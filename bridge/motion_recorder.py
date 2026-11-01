#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""家庭摄像头录像服务

两路：
  1. 客厅 —— 移动侦测触发（子码流检测，有人才录主码流）
  2. 门口机 —— 振铃触发（轮询 HA 的「呼叫中」二进制传感器，有人按铃才录）

容量策略：
  - 客厅目录硬上限 1TB（超过就删最旧的）
  - 门口机目录保留 180 天
  - 两者都额外受「保留天数」约束

所有参数走环境变量，改完 docker compose up -d 生效。
"""
import datetime
import os
import signal
import subprocess
import sys
import threading
import time
import urllib.request
import json

# ---------- 通用配置 ----------
OUT_ROOT = os.environ.get("OUT_DIR", "/rec/go2rtc")
CAP_KB = int(os.environ.get("CAP_KB", str(1024 * 1024 * 1024)))        # 客厅上限：1TB
DOOR_CAP_KB = int(os.environ.get("DOOR_CAP_KB", str(100 * 1024 * 1024)))  # 门口机上限：100GB

# ---------- 客厅：移动侦测 ----------
DETECT_URL = os.environ.get("DETECT_URL", "rtsp://127.0.0.1:8554/keting_sub")
LIVING_URL = os.environ.get("REC_URL", "rtsp://127.0.0.1:8554/keting")
LIVING_DIR = os.path.join(OUT_ROOT, "客厅")

W, H = 160, 120
FPS = 2
PIX_TH = int(os.environ.get("PIX_TH", "28"))
RATIO_TH = float(os.environ.get("RATIO_TH", "0.006"))
START_N = int(os.environ.get("START_N", "2"))
TAIL_S = int(os.environ.get("TAIL_S", "30"))
MIN_S = int(os.environ.get("MIN_S", "30"))
MAX_S = int(os.environ.get("MAX_S", "600"))
DEBUG = os.environ.get("DEBUG", "0") == "1"

# ---------- 门口机：振铃触发 ----------
HA_URL = os.environ.get("HA_URL", "http://192.168.1.11:8123")
HA_TOKEN = os.environ.get("HA_TOKEN", "")
RING_ENTITY = os.environ.get(
    "RING_ENTITY", "binary_sensor.men_jin_qiao_jie_qi_men_kou_ji_hu_jiao_zhong_ringing")
STATUS_ENTITY = os.environ.get(
    "STATUS_ENTITY", "sensor.men_jin_qiao_jie_qi_men_jin_zhuang_tai_status")
DOOR_URL = os.environ.get("DOOR_URL", "rtsp://127.0.0.1:8554/menjin")
DOOR_DIR = os.path.join(OUT_ROOT, "门口机")
DOOR_MAX_S = int(os.environ.get("DOOR_MAX_S", "180"))   # 单次最长 3 分钟
DOOR_TAIL_S = int(os.environ.get("DOOR_TAIL_S", "8"))   # 挂断后补录


def log(msg):
    print("[rec] " + msg, flush=True)


def start_rec(rtsp_url, out_dir, max_s, vtag=None):
    """vtag：HEVC 流用 hvc1，H.264 流不能加（否则报
    'Tag hvc1 incompatible with output codec id 27 (avc1)' 并写出 0 字节文件）。"""
    os.makedirs(out_dir, exist_ok=True)
    name = datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + ".mp4"
    path = os.path.join(out_dir, name)
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "warning",
        "-rtsp_transport", "tcp", "-i", rtsp_url, "-t", str(max_s),
        "-map", "0:v", "-map", "0:a?",
        "-c:v", "copy",
    ]
    if vtag:
        cmd += ["-tag:v", vtag]
    cmd += [
        "-c:a", "aac", "-b:a", "48k", "-ar", "16000", "-ac", "1",
        "-movflags", "+frag_keyframe+empty_moov+default_base_moof",
        "-y", path,
    ]
    return subprocess.Popen(cmd), path


def stop_rec(proc):
    if proc is None:
        return
    try:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=15)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


# ==================== 客厅：移动侦测 ====================
def start_detector():
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-rtsp_transport", "tcp", "-i", DETECT_URL,
        "-vf", "fps=%d,scale=%d:%d,format=gray" % (FPS, W, H),
        "-f", "rawvideo", "-pix_fmt", "gray", "-",
    ]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=W * H * 4)


def living_worker():
    log("客厅：启动移动侦测 检测=%s 录像=%s 输出=%s" % (DETECT_URL, LIVING_URL, LIVING_DIR))
    frame_size = W * H
    sample = list(range(0, frame_size, 7))
    det = None
    rec = None
    rec_started = 0.0
    last_motion = 0.0
    hit = 0
    prev = None
    t_stat = time.time()
    max_ratio = 0.0

    while True:
        if det is None or det.poll() is not None:
            if det is not None:
                log("客厅：检测流断开，重连")
            det = start_detector()
            prev = None
            time.sleep(1)

        buf = det.stdout.read(frame_size)
        if not buf or len(buf) < frame_size:
            time.sleep(0.2)
            continue

        if prev is not None:
            changed = 0
            for i in sample:
                d = buf[i] - prev[i]
                if d > PIX_TH or d < -PIX_TH:
                    changed += 1
            ratio = changed / float(len(sample))
            if ratio > max_ratio:
                max_ratio = ratio

            if ratio >= RATIO_TH:
                hit += 1
                last_motion = time.time()
            else:
                hit = 0

            if DEBUG and ratio > RATIO_TH * 0.5:
                log("客厅：ratio=%.4f hit=%d rec=%s" % (ratio, hit, rec is not None))

            if rec is None and hit >= START_N:
                rec, path = start_rec(LIVING_URL, LIVING_DIR, MAX_S, "hvc1")
                rec_started = time.time()
                log("客厅：检测到移动，开始录像 %s" % os.path.basename(path))

        prev = buf

        if rec is not None:
            elapsed = time.time() - rec_started
            idle = time.time() - last_motion
            if rec.poll() is not None:
                rec = None
                if idle < TAIL_S:
                    rec, path = start_rec(LIVING_URL, LIVING_DIR, MAX_S, "hvc1")
                    rec_started = time.time()
                    log("客厅：达到最长时长，续录 %s" % os.path.basename(path))
                else:
                    log("客厅：本段结束")
            elif idle >= TAIL_S and elapsed >= MIN_S:
                stop_rec(rec)
                rec = None
                log("客厅：人已离开，停止录像（%.0fs）" % elapsed)

        if time.time() - t_stat >= 60:
            log("客厅：录制中=%s 本分钟峰值 ratio=%.4f" % (rec is not None, max_ratio))
            max_ratio = 0.0
            t_stat = time.time()


# ==================== 门口机：振铃触发 ====================
def ha_state(entity_id):
    if not HA_TOKEN:
        return None
    req = urllib.request.Request(
        "%s/api/states/%s" % (HA_URL, entity_id),
        headers={"Authorization": "Bearer " + HA_TOKEN})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read().decode("utf-8")).get("state")
    except Exception:
        return None


def door_worker():
    if not HA_TOKEN:
        log("门口机：未配置 HA_TOKEN，跳过")
        return
    log("门口机：监听振铃 实体=%s 输出=%s" % (RING_ENTITY, DOOR_DIR))
    rec = None
    started = 0.0
    last_active = 0.0
    while True:
        ringing = ha_state(RING_ENTITY) == "on"
        status = ha_state(STATUS_ENTITY) or ""
        active = ringing or status in ("振铃", "通话中")

        if active:
            last_active = time.time()
            if rec is None:
                rec, path = start_rec(DOOR_URL, DOOR_DIR, DOOR_MAX_S, None)
                started = time.time()
                log("门口机：有人按铃/通话中，开始录像 %s" % os.path.basename(path))
        elif rec is not None:
            elapsed = time.time() - started
            idle = time.time() - last_active
            if idle >= DOOR_TAIL_S:
                stop_rec(rec)
                rec = None
                log("门口机：录像结束（%.0fs）" % elapsed)

        if rec is not None and time.time() - started >= DOOR_MAX_S:
            stop_rec(rec)
            rec = None
            log("门口机：达到最长时长，本段结束")

        time.sleep(3)


# ==================== 容量管理 ====================
def dir_stats(path):
    total = 0
    files = []
    if not os.path.isdir(path):
        return 0, files
    for f in os.listdir(path):
        if not f.endswith(".mp4"):
            continue
        p = os.path.join(path, f)
        try:
            st = os.stat(p)
        except OSError:
            continue
        total += st.st_size
        files.append((st.st_mtime, p))
    files.sort()
    return total, files


def enforce_cap(path, limit_bytes, label):
    """超过容量上限就从最旧的开始删，直到低于上限。返回当前总字节数。"""
    total, files = dir_stats(path)
    while total > limit_bytes and files:
        mt, p = files.pop(0)
        try:
            sz = os.path.getsize(p)
            os.remove(p)
            total -= sz
            log("%s 超出容量上限，删除最旧：%s" % (label, os.path.basename(p)))
        except OSError:
            pass
    return total


def cleanup_worker():
    """只按容量清理，不按时间。每小时检查一次。"""
    while True:
        time.sleep(3600)
        t1 = enforce_cap(LIVING_DIR, CAP_KB * 1024, "客厅")
        t2 = enforce_cap(DOOR_DIR, DOOR_CAP_KB * 1024, "门口机")
        log("容量：客厅 %.1f GB / %.0f GB，门口机 %.1f GB / %.0f GB"
            % (t1 / 1073741824.0, CAP_KB / 1048576.0,
               t2 / 1073741824.0, DOOR_CAP_KB / 1048576.0))


def main():
    os.makedirs(LIVING_DIR, exist_ok=True)
    os.makedirs(DOOR_DIR, exist_ok=True)
    log("启动：客厅上限 %.0f GB，门口机上限 %.0f GB（只按容量清理，不按时间）"
        % (CAP_KB / 1048576.0, DOOR_CAP_KB / 1048576.0))
    threading.Thread(target=living_worker, daemon=True).start()
    threading.Thread(target=door_worker, daemon=True).start()
    threading.Thread(target=cleanup_worker, daemon=True).start()
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
