#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""安居宝门口机视频桥（带待机画面）。

WT32-ETH01 把门口机的视频流（UDP/RTP，载荷是裸 H.264 Annex-B 字节流的
约 1KB 切片）转发到本程序。本程序剥掉每个包 12 字节的 RTP 头，把切片按序
拼接成完整的裸 H.264 流，从 stdout 输出。

容器里用管道接 ffmpeg，封装成 RTSP 推给 go2rtc，最终进入 Home Assistant：

    门口机 --9880--> WT32-ETH01 --WiFi--> 本程序 --stdout--> ffmpeg --RTSP--> go2rtc --> HA

抓包确认的流特征（14.json）：
  - 每个 UDP 包 = 12 字节 RTP 头（CC=0、无扩展、PT=98）+ 裸流切片
  - 切片无 FU-A，直接按序拼接即可还原 Annex-B 字节流
  - H.264 Baseline 3.1，640x480 @ 25fps，约 1.24 Mbps

待机画面：门口机只在呼叫/监视时推视频。若空闲时完全不输出，go2rtc 里没有
这个流，HA 添加摄像头会探测超时、面板上也是"不可用"。因此空闲超过 3 秒后
本程序以约 2fps 注入一帧预生成的"门禁待机中"画面（H.264 Baseline 3.1
640x480 IDR，与门口机参数一致），让流永远在线；实时视频一到立即无缝切换。
"""

import asyncio
import base64
import collections
import os
import socket
import ssl
import sys
import threading
import time

try:
    from aiohttp import web, ClientSession
    HAS_AIOHTTP = True
except ImportError:
    HAS_AIOHTTP = False

LISTEN_PORT = int(os.environ.get("LISTEN_PORT", "9880"))
ALLOWED_SRC = os.environ.get("ALLOWED_SRC", "")  # 只收这个 IP 的包（WT32-ETH01 的 WiFi IP），空=不限制

IDLE_TIMEOUT_S = 3.0      # 超过这么久没收到实时包，进入待机
STANDBY_INTERVAL_S = 0.5  # 待机帧输出间隔（约 2fps，足够保活/预览）

# 开机/重连后的探测窗口：ffmpeg 要收满一定数据才完成流探测、才去连 go2rtc，
# 而探测时长取决于"收到的帧数"——2fps 待机帧实测要 40 秒以上才探测完，
# 这期间 go2rtc 里流一直离线（面板报 streams: unknown error）。
# 因此启动后的前 FAST_STANDBY_S 秒把待机帧提到 25fps，让 ffmpeg 几秒内完成
# 探测上线，之后再回落到 2fps 保活。
FAST_STANDBY_S = 20.0
STARTUP_STANDBY_FPS = 25.0  # 探测窗口内的待机帧率（窗口外由 STANDBY_INTERVAL_S 决定，约 2fps）

# ---- 二期：双向音频对讲 ----
# 音频线格式（抓包实测）：裸 G.711 A-law，8kHz，256 字节/包，约 32ms 一包，无 RTP 头。
#   下行：门口机:6668 → ESP32 → 本程序 :9990 →（转）→ ffmpeg udp://127.0.0.1:9991 → RTSP(PCMA) → go2rtc → 浏览器
#   上行：iPad 按住说话 →（HTTPS 页面 A-law 编码）→ WebSocket → 本程序 → ESP32:6670 → 门口机:6668
AUDIO_LISTEN_PORT = int(os.environ.get("AUDIO_LISTEN_PORT", "9990"))   # 收 ESP32 转来的访客语音
AUDIO_FFMPEG_PORT = int(os.environ.get("AUDIO_FFMPEG_PORT", "9991"))   # 转给本机 ffmpeg 的 UDP
ESP32_IP = os.environ.get("ESP32_IP", "192.168.1.48")                # ESP32（WiFi 侧）
ESP32_AUDIO_PORT = int(os.environ.get("ESP32_AUDIO_PORT", "6670"))     # ESP32 上行音频入口
PANEL_PORT = int(os.environ.get("PANEL_PORT", "8443"))                 # 对讲面板 HTTPS 端口
CERT_DIR = os.environ.get("CERT_DIR", "/certs")                        # server.crt / server.key
GO2RTC_HTTP = os.environ.get("GO2RTC_HTTP", "127.0.0.1:1984")          # 本机 go2rtc（面板里代理它）

# 预生成的待机帧：640x480 深灰底 + "门禁待机中" 提示，单 IDR（含 SPS/PPS）
STANDBY_FRAME = base64.b64decode("""AAAAAWdCwB/ZAKA9sBEAAAMAAQAAAwAyDxgySAAAAAFoy4PLIAAAAQYF//9t3EXpvebZSLeWLNgg
2SPu73gyNjQgLSBjb3JlIDE2NCByMzA5NSBiYWVlNDAwIC0gSC4yNjQvTVBFRy00IEFWQyBjb2Rl
YyAtIENvcHlsZWZ0IDIwMDMtMjAyMiAtIGh0dHA6Ly93d3cudmlkZW9sYW4ub3JnL3gyNjQuaHRt
bCAtIG9wdGlvbnM6IGNhYmFjPTAgcmVmPTMgZGVibG9jaz0xOjA6MCBhbmFseXNlPTB4MToweDEx
MSBtZT1oZXggc3VibWU9NyBwc3k9MSBwc3lfcmQ9MS4wMDowLjAwIG1peGVkX3JlZj0xIG1lX3Jh
bmdlPTE2IGNocm9tYV9tZT0xIHRyZWxsaXM9MSA4eDhkY3Q9MCBjcW09MCBkZWFkem9uZT0yMSwx
MSBmYXN0X3Bza2lwPTEgY2hyb21hX3FwX29mZnNldD0tMiB0aHJlYWRzPTMgbG9va2FoZWFkX3Ro
cmVhZHM9MSBzbGljZWRfdGhyZWFkcz0wIG5yPTAgZGVjaW1hdGU9MSBpbnRlcmxhY2VkPTAgYmx1
cmF5X2NvbXBhdD0wIGNvbnN0cmFpbmVkX2ludHJhPTAgYmZyYW1lcz0wIHdlaWdodHA9MCBrZXlp
bnQ9MjUwIGtleWludF9taW49MjUgc2NlbmVjdXQ9NDAgaW50cmFfcmVmcmVzaD0wIHJjX2xvb2th
aGVhZD00MCByYz1jcmYgbWJ0cmVlPTEgY3JmPTIzLjAgcWNvbXA9MC42MCBxcG1pbj0wIHFwbWF4
PTY5IHFwc3RlcD00IGlwX3JhdGlvPTEuNDAgYXE9MToxLjAwAIAAAAFliIQK8RigAChjHAMcAZOT
k5OTk5OTk5OTk5OTk5OTk5OTk5OTk5OTk5OTk5OTk5OTk5OTk666666666666666666666666666
6666666666666666666666666666666666666666666666666666666666666666666666666666
6666666666666666666666666666666666666666666666666666666666666666666666666666
6666666666666666666666666666666666666666666666666666666666666666666666666666
6666666666666666666666666666666666666666666666666666666666666666666666666666
6666666666666666666666666666666666666666666666666666666666666666666666666666
6666666666666666666666666666666666666666666666//+IYIQTADWtjYbO13Ume9f/wui4gm
B0MJQXAMTOHRFZeQv68BLvgo1EiOZGX/4+YYVszzgl1l//EOGCjgwATIBghImJaYWAAQ4C0zE7a/
+LRiMR4GpgBJkGAOQATYGpXU+AOeAksxwCPX7//1COL4eF6fAR/sZsdkA0gMUwCuMyNncw0uXjnI
iMhPfARGZkgIioUzMea8Ddh3SCERIVRitB31QyqvPf5DzJdDL+ZGXjphqMK41QsAAlwQGv3+uIrf
1N5m8Fjw+//A78XiPAdMMMmAJ1yT+P0iAEX6vMg+2dePjmE++OglPAVj6dlDQ8BL76+6MV9Iw1iQ
dT8/+j/nh/CvP7JlM5F7//D+CCAj3zpdy12mfqxeyBHz8DRDCJgdC1CDNsql055e2/f/6w9ggK4I
msS41BPXXXXXXXXXXXXXXXXXXXXXXXXXCUFH7HyP+R8jzWH0fx6BH491wCAHdLo0qbJ7/v//4I52
UP1kQK5AEVEfUCG7nzf2AucV7AE5REYz5wzX0JwIgXmLtW/bZ05NwTXfgOj5pYfGRJbn/9SzOP/C
Wd6fQ41uGDdNLf+4tsiVk1MQ+JGnEbfX5ohx+BUf8Uu1I2VFPCAsiVcbKv2hqDNYuSPIq3UAPQKL
lPL/ARUNyAR6+tz18c+RfHgTNMqhDUl73Zg15YItTJYIT6GSGJGmARHm3PboOuStKIZ9a/PIgcQY
DAtY1NnjZ2yVbs1Bac3Wv8FmHuijGYVv6m+RfgJRkJxsIlhQo2lvjjyItdrWkHRNrQCg2EWkdaPT
3n0/clBHsV5hC8sH277D0ImcOsjSJy99gVGGuqZlQ/Si7bhBfnIhibTXLmsu/zqAEfrjrFxo6NyE
UQ0ewVW2WIEimL3TIxtpYK1lFe//gH1EOuO05YABf64HgcZYPGR5bvj/ZFMA54GiGiYHQRaeBC/v
lMbmWumS+oeOzvTKbQpUwU1111111111111111111111111/h/wCQzgOTAAJEwcAAjfD0jZucFGA
DEPfEOoOA8aHfAorqb76LnZgwJZ1wUpdECM0GowfXoCbheKakbIssiPHWXCygFFO7TrTFLNv3mAR
/BtK3+4BnR2F2J9RN9R3PcAY6yFhwyoRTc4145fFkVbqCFsr8ffLzNiKbxDxMRgojNBeChPQJeR9
3D6CEJF0vH8OoTwCOg/5gFSHNdWZgI/SAt3TNgFPMiz0k8C1HdE+Jepx/mKCJ+Pe/if1X38Ug4f8
IW+0+5fb5x/EKfuo/drEzHQJ+p92xJ4o9SPA1lJkD2xAlWz7f5JfP/2HE2A0xbhCBwP9BGNXkC4d
8wnyJmBRSqf+J25Zr+r/6CMP/oLHvathPnn9/Dwf5TA10zlp9/XO1cP/8PHAKEiRTLD36YI66666
666666666666666666666WulpaW1tbW1tbW1tbWuuuuuuuuuuuuuuuuuuuuv/4obqAQyCQEj3Yr2
uBFi/NAjGrqFfy5iUdL6/8VtwhXVAhaczuARVsbDrx/8/GEK4nqv/p4kYJHuweyZURYR//AxwhWE
3vjmHLqgvOHdjX4LgpmER32Q2f96R/4ioP4Vh7TIQLNIbmG4s80ZNFlTIGA2w3Wyjn/qKDGcKzT+
0Ora1ngSWAjBuAukXDj193/dBYfCsdbdJ/1Xekm7xllqCohS2j3HCoHoP/jTCFeUBtAeQh0yBDDp
JNmvTHUv+ML4QrvfhcVF94KOtzCZpwd7//+IwrATuyCDGQmpryM69BDbMCroHpD8sN35TtIqkz/+
ruMRGFeAMNz2CgBldzWWuy7Eu87fMPNT2NzUnUJEv/6BFQCFbXvn4Y9+rQfVJmrDCmSPFSgVoNm5
qPf/AQvhXgcGmptYPaLSwCfI9dhjaafWHf9zCIJhXGqb+dNVVhBn783AUGtlVx13/CF0AYVgeGzI
z/pi94pHXrJAjmRZUqc3Z/9biowrgEMd8ImkJg029GLYtI2sySStqaJ01RYDWf/OHCFeE338DAHU
QU10OAmYAZbkh8kWkTdnVf91YgwhWVkSTEVaUKbG6ctLOvwCtswsI6pUcX3/EFHDCuiApo54ukYa
DZLQ34LPYlvWf8HUlF1hWHmTIQf5IxMjKNPN5aZWWMFq2S9FaWz7b+f/xEAUACFY2/SKKZb8oFaG
epsbNzUe2v/xuO8KwODTUEZW0uwekLSF7zTtSUYxX6WhfWbl//8REYV4Be5T4yDOVU3oDpM5VtXA
ANDgEj/93nKFfBD+rCCOLJod6nfNMEtddddddddddddddeU1eAQhENAmHTKr/NjLemT05mY5gUN5
dfazkRD0BO2a++Bdm2CZdz0fgn2jBk3LzX+YY6z/pIV9M+Ano/fLc3wOI8CyS9elgKdabts+rlkN
tk7iG5Sowa3V3LLpPAqRP59pmu9TeifD3wbvTFVBCtwP/H/jiLjwhN+IlknupyKLHjCiljAWflp6
/4+N5hkPfrAnxVUBdhEIsyUbrh5iLZVTi+xV57X6m+o8Vm7q2/62NlmDI2XvXURkX9uq51n39KDd
XYiL04Yuhl/dBsN1t//5+JhOA4Az81HOwGDZ30zlpYFMqKzzv+Zl5vF+AL7U3NyWvj4f344X+FUl
NN2Q2Sm95rNlTZE1E/B1tUhL3bq/piCjD/UBPiX3sVbk3vJdfaBOjRVTPUxDTiUQJ0aKqwNW6q+c
lP03N8/Tc0Vq6m/vmyIEGCAE+nB7p0nnQxp+yvZ/t1MfL7lczpYaebN4CDd1Hb/z/lCk8HDrxnWm
Q/6eGpKpdFQu10azwTMM95+DP8WYL8m8AuEv0IbrvdLo6Q3Ug9dhVnFBduy/MWyvCEEw9A1Ztl5W
Wg7ph1BmldrjriG/fIzTKsa++bs+0X4aUZdegZa+08W9Na86XBPrJi243ZsYrFbsfy/DhOvZY+uN
BB2Ysulj5HRgGJnsEbpK06p+I916C+48eD5mJ+0DKvSbv+/kKu2x98Sd1Cry6PsgprckDN69QGr1
z1d9b7O3q/P9lFWit7+EUhumY9+CavcCeoipV+cdoUl0JLMW9YY5/LpldTzwqy+yMgFy2Huq+R2K
nNAysjwrfJnp+zIIfdj2IU1b5v5ov/YCAXk+K9RbZffPOHvzIyM1RSHnfhx7rx6IRZkC4krAzLs/
Oe/Ck6o+B9w+piqSicjXaRdarogl+hDdu5zcbnY9+FRMWV/wg2A+I+HpjUd4gnfUwXby/wLlJf98
Q48PALkUxq+Q+3MtuSeBU/FVd8B7NUv+GAeGovtLKVW3TA9tUjrOTKhUGJn+RVc9x94IzRZX3pgn
rrrrrrrrrrrrrrrpaWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlrrrrrrrrrrrrrrrrrrrrrrrrrrrrr
rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr
rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr
rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr
rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr
rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr
rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrw""")


def log(msg):
    print(f"[video-bridge] {msg}", file=sys.stderr, flush=True)


# 视频 dts 估计（ffmpeg 以 -framerate 25 计数：每组装出一帧 dts +40ms）。
# 音频补静音必须跟着它走——两路时间戳谁跑太快都会把对方卡死在混流缓冲里。
VIDEO_DTS = [0.0]   # 主循环更新，音频线程读取
FRAME_DTS = 1.0 / 25

# go2rtc 流健康状态：menjin 有生产者 = ffmpeg 已把流推上来了。
# 音频"静音领先视频 dts"的额度据此分两档——
#   探测期（流未建立）：ffmpeg 逐个探测输入时视频管道被堵、视频 dts 冻结，
#     需要 ~9s 的大额度让音频探测渡过死锁窗口（双输入实测铁律）；
#   稳态（流已建立）：额度必须小——否则待机时音频领先会一路攒到上限，
#     呼叫开始时播放器为了对齐音画会把声音压后同样时长（实测 10 秒级延迟）。
#   ffmpeg 意外重启（流掉线）时自动回到 9s 档。
STREAM_OK = [False]


def go2rtc_watch_thread():
    import json as _json
    import urllib.request
    while True:
        ok = False
        try:
            with urllib.request.urlopen("http://%s/api/streams" % GO2RTC_HTTP, timeout=3) as _r:
                _d = _json.load(_r)
            ok = bool(_d.get("menjin", {}).get("producers"))
        except Exception:
            ok = False
        STREAM_OK[0] = ok
        time.sleep(3)


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind(("0.0.0.0", LISTEN_PORT))
    sock.settimeout(STANDBY_INTERVAL_S)
    log(f"监听 UDP :{LISTEN_PORT}" + (f"，只收 {ALLOWED_SRC}" if ALLOWED_SRC else "（来源不限）"))

    # 二期：音频下行线程 + 对讲面板线程（与视频主循环并行）
    threading.Thread(target=audio_downlink_thread, daemon=True, name="audio-downlink").start()
    threading.Thread(target=go2rtc_watch_thread, daemon=True, name="go2rtc-watch").start()
    if HAS_AIOHTTP:
        threading.Thread(target=panel_thread, daemon=True, name="panel").start()
    else:
        log("警告：未安装 aiohttp，对讲面板不可用（pip install aiohttp）")

    out = sys.stdout.buffer
    last_seq = None
    warned_src = set()
    n_pkts = n_bytes = 0
    t_stat = time.monotonic()
    last_live = 0.0        # 最近一次收到实时包的时间（0 表示从未收到）
    standby_on = False
    t_start = time.monotonic()
    next_standby = 0.0     # 下一帧待机画面的到期时间
    fast_window = True     # 启动后的快速待机窗口（帮助 ffmpeg 探测）
    standby_interval = 1.0 / STARTUP_STANDBY_FPS
    sock.settimeout(standby_interval)
    log(f"探测窗口：前 {FAST_STANDBY_S:.0f}s 待机帧 {STARTUP_STANDBY_FPS:.0f}fps，之后降为 2fps")

    def write_frame(buf):
        try:
            out.write(buf)
            out.flush()
            return True
        except BrokenPipeError:
            log("ffmpeg 管道断开，退出（容器会自动重启重连）")
            return False

    while True:
        if fast_window and time.monotonic() - t_start >= FAST_STANDBY_S:
            fast_window = False
            standby_interval = STANDBY_INTERVAL_S
            sock.settimeout(standby_interval)
            if standby_on:
                log("探测窗口结束，待机画面降为 2fps（流保持在线）")
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            data = None

        now = time.monotonic()

        if data is None:
            # 空窗：空闲超时则补待机画面，保持流常开。用"下一次到期时间"调度：
            # 管道堵塞（ffmpeg 探测音频时不读视频管）解除后也不会倾泻积压帧。
            if now - last_live > IDLE_TIMEOUT_S:
                if not standby_on:
                    standby_on = True
                    next_standby = now
                    log("画面空闲，注入待机帧（实时视频到达后自动切回）")
                if now >= next_standby:
                    if not write_frame(STANDBY_FRAME):
                        return
                    VIDEO_DTS[0] += FRAME_DTS   # 待机帧也是一帧
                    next_standby = time.monotonic() + standby_interval
            continue

        if ALLOWED_SRC and addr[0] != ALLOWED_SRC:
            if addr[0] not in warned_src:
                warned_src.add(addr[0])
                log(f"忽略非授权来源 {addr[0]}（不再重复提示）")
            continue
        # 必须是 RTP v2 包且至少 13 字节（12 字节头 + 1 字节载荷）
        if len(data) < 13 or (data[0] >> 6) != 2:
            continue
        seq = int.from_bytes(data[2:4], "big")
        if last_seq is not None and seq != (last_seq + 1) & 0xFFFF:
            log(f"丢包/乱序：seq {last_seq} -> {seq}（解码器在下个起始码自动重新同步）")
        last_seq = seq
        hdr = 12 + 4 * (data[0] & 0x0F)  # 抓包确认 CC=0，仍按规范计算
        if not write_frame(data[hdr:]):
            return
        if data[1] & 0x80:               # RTP marker=一帧的最后一个包 → ffmpeg 侧将组装出一帧
            VIDEO_DTS[0] += FRAME_DTS
        last_live = now
        if standby_on:
            standby_on = False
            log("收到实时视频，切换为门口机画面")
        n_pkts += 1
        n_bytes += len(data)
        if now - t_stat >= 5.0:
            if n_pkts:
                kbps = n_bytes * 8 / (now - t_stat) / 1000
                log(f"转发中：{n_pkts} 包 / {now - t_stat:.0f}s ≈ {kbps:.0f} kbps")
            n_pkts = n_bytes = 0
            t_stat = now


# ======================= 二期：双向音频对讲 =======================

def audio_downlink_thread():
    """下行音频：ESP32 转来的门口机语音（UDP :9990，白名单过滤）→ 本机 ffmpeg（udp://127.0.0.1:9991）。

    ffmpeg 双输入实测（5.1.9 / 7.0.2 行为一致）有两条铁律：
    1) 音频干线一旦断流，整个主循环连视频也一起冻结（输入端按 dts 顺序读流，
       落后那路读不到包就停等）。所以静音补偿永远不能完全停；
    2) 但音频 dts 领先视频 dts 超过混流器容忍上限（max_interleave_delta≈10s）
       又会反向把视频卡死。
    两路输入都是"计数时间戳"（视频每帧 +40ms，音频每字节 1/8000s），待机时
    视频 dts 走得比墙钟慢（2fps 时只有 0.08s/s）。因此规则定为：
    静音只许领先视频 dts 最多 AUDIO_LEAD_S 秒——待机时自动退化为细流
    （约 2.5 包/秒，不会断），通话/探测期自动跟上实时，两头都不卡。
    （ffmpeg 是逐个打开并探测输入的：先探测视频约 5 秒，期间音频口还没开；
    开始探测音频时主线程已被视频管道堵住、视频 dts 冻结——+9s 的额度足够
    音频探测一次拿满约 2 秒数据，死锁不会发生。）"""
    # 静音允许领先视频 dts 的上限（必须 < 混流器 10s 容忍），按 go2rtc 流健康分两档：
    # 流未建立（ffmpeg 探测期）给 9s 大额度防双输入死锁；流已建立给 1.2s——
    # 否则待机时领先攒到上限，呼叫时播放器对齐音画会把声音压后 ~10 秒。
    def lead_cap():
        return 1.2 if STREAM_OK[0] else 9.0
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    s.bind(("0.0.0.0", AUDIO_LISTEN_PORT))
    s.settimeout(0.032)   # 32ms 节奏检查
    out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dst = ("127.0.0.1", AUDIO_FFMPEG_PORT)
    silence = bytes([0xD5]) * 256
    log(f"下行音频线程就绪：UDP :{AUDIO_LISTEN_PORT} → ffmpeg 127.0.0.1:{AUDIO_FFMPEG_PORT}")
    n_real = n_sil = 0
    audio_dts = 0.0       # 已发给 ffmpeg 的音频时长（字节数/8000）
    t0 = time.monotonic()
    warned = set()
    while True:
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            data = None
        if data is not None:
            if ALLOWED_SRC and addr[0] != ALLOWED_SRC:
                if addr[0] not in warned:
                    warned.add(addr[0])
                    log(f"忽略非授权音频来源 {addr[0]}（不再重复提示）")
                continue
            out.sendto(data, dst)          # 真实语音永远转发（通话中门口机 32ms 一包，与实时同速）
            audio_dts += len(data) / 8000.0
            n_real += 1
        elif audio_dts < VIDEO_DTS[0] + lead_cap():
            out.sendto(silence, dst)       # 没人说话：补静音，领先视频 dts 不超过当前档位上限
            audio_dts += 256 / 8000.0
            n_sil += 1
        now = time.monotonic()
        if now - t0 >= 10.0:
            if n_real or n_sil:
                log(f"下行音频：真实 {n_real} + 静音 {n_sil} 包 / 10s")
            n_real = n_sil = 0
            t0 = now


# ---- 对讲面板（HTTPS）----
# 单文件页面：内嵌 go2rtc 播放器（经本站代理，同源）+ 按住说话按钮。
# 按住：麦克风 → 重采样 8kHz → A-law 编码 → 256 字节二进制帧经 WebSocket 发回本站。
# 松开：补发 3 包静音（0xD5），恢复视频声音。
INTERCOM_HTML = r"""<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<title>门禁对讲</title>
<style>
html,body{margin:0;height:100%;background:#0b0e14;color:#e8eaf0;font-family:-apple-system,"PingFang SC",sans-serif;overscroll-behavior:none}
.wrap{display:flex;flex-direction:column;height:100%;max-width:900px;margin:0 auto}
h1{font-size:17px;font-weight:600;text-align:center;margin:12px 0 8px}
.video{flex:1;min-height:0;margin:0 12px;border-radius:14px;overflow:hidden;background:#000}
.video iframe{width:100%;height:100%;border:0;display:block}
.bar{padding:18px 0 28px;display:flex;flex-direction:column;align-items:center;gap:10px}
#ptt{width:120px;height:120px;border-radius:50%;border:none;font-size:18px;font-weight:600;color:#fff;background:#2b6cb0;touch-action:none;user-select:none;-webkit-user-select:none;-webkit-tap-highlight-color:transparent;transition:transform .06s,background .06s}
#ptt.on{background:#e53e3e;transform:scale(1.08)}
#st{font-size:13px;color:#9aa3b2;min-height:18px}
#hint{font-size:12px;color:#68707e;text-align:center;line-height:1.6;padding:0 24px 12px}
.cmds{display:flex;gap:14px}
.cmds button{width:150px;height:46px;border-radius:12px;border:none;font-size:16px;font-weight:600;color:#fff;touch-action:manipulation;user-select:none;-webkit-user-select:none;-webkit-tap-highlight-color:transparent}
#btnUnlock{background:#2f855a}
#btnHangup{background:#c53030}
</style>
</head>
<body>
<div class="wrap">
<h1>门禁对讲</h1>
<div class="video"><iframe id="vf" src="/stream.html?src=menjin&amp;mode=webrtc" allow="autoplay;camera;microphone"></iframe></div>
<div class="bar">
<button id="ptt">按住 说话</button>
<div class="cmds">
<button id="btnUnlock">🔓 开门解锁</button>
<button id="btnHangup">挂断通话</button>
</div>
<div id="st">连接中…</div>
</div>
<div id="hint">按住按钮对门口机讲话，松开收听。<br>画面第一次不会自动出声时，请先点一下画面。</div>
</div>
<script>
var st=document.getElementById('st'),btn=document.getElementById('ptt');
var proto=location.protocol==='https:'?'wss://':'ws://';
var ws=null,talking=false,busy=false;
var ctx=null,node=null,gain=null,workletReady=false,sentN=0,encN=0,hbN=0,micTrack=null,tickTimer=null,lastRms=null;

function setSt(t){st.textContent=t;}
function report(t){try{if(ws&&ws.readyState===1)ws.send('PAGE '+t);}catch(e){}}
var PAGE_VER='v17';
window.onerror=function(m,src,l,c){setSt('页面错误：'+m);report('ERROR '+m+' @'+(src||'')+':'+l+':'+c);};
function wsState(){return (ws&&ws.readyState===1)?'已连接，等待呼叫':'连接中…';}
// 状态行全文：引擎心跳（process 是否被拉动）/ 编码（worklet 产出的包）/ 已发（成功送出的包）
function stText(){
  var s='通话中（上行）';
  s+=' · 引擎跳'+Math.floor(hbN/256);
  s+=' · 编码'+encN+' · 已发'+sentN;
  if(talking&&!(ws&&ws.readyState===1))s+=' · 连接断开未发出';
  if(ctx)s+=' · '+ctx.state;
  return s;
}
function connect(){
  try{ws=new WebSocket(proto+location.host+'/ws/talk');}catch(e){setSt('连接失败');return;}
  ws.binaryType='arraybuffer';
  ws.onopen=function(){setSt(wsState());var m=navigator.userAgent.match(/OS (\d+_\d+ like Mac OS X|Android [\d.]+)/);report('HELLO '+PAGE_VER+' os='+(m?m[1]:'?')+' lang='+navigator.language);};
  ws.onclose=function(){setSt('连接断开，1 秒后重连…');setTimeout(connect,1000);};
  ws.onerror=function(){try{ws.close();}catch(e){}};
}
connect();

// A-law 编码 + 重采样到 8kHz（跑在 AudioWorklet 线程）
var WORKLET=`
var SEG_END=[0xFF,0x1FF,0x3FF,0x7FF,0xFFF,0x1FFF,0x3FFF,0x7FFF];
var MIC_GAIN=30;   // 原始麦克风增益（禁用系统 AGC 改软件放大；实测 iPad 原始语音 RMS 约-47dB，+29dB 后到门口机音量充足）
function alaw(v){
  var x=Math.round(Math.max(-1,Math.min(1,v))*32767),mask;
  if(x>=0){mask=0xD5;}else{mask=0x55;x=-x-8;if(x<0)x=0;}
  var seg=0;while(seg<8&&x>SEG_END[seg])seg++;
  if(seg>=8)return 0x7F^mask;
  var a=seg<<4;
  a|=(seg<2)?((x>>4)&15):((x>>(seg+3))&15);
  return (a^mask)&255;
}
class P extends AudioWorkletProcessor{
  constructor(){super();this.step=sampleRate/8000;this.pos=0;this.buf=new Float32Array(4096);this.n=0;this.out=new Uint8Array(256);this.m=0;this.pn=0;this.acc=0;this.cnt=0;}
  process(inp){
   try{
    this.pn++;
    if(this.pn%256===0)this.port.postMessage('hb'+this.pn);   // 引擎心跳：证明 process 还在被渲染引擎拉动
    var c=inp[0]&&inp[0][0];
    if(c&&c.length){
      if(c.length>this.buf.length){c=c.subarray(0,this.buf.length);}   // 极端大帧防溢出
      if(this.n+c.length>this.buf.length){this.n=0;this.pos=0;}
      this.buf.set(c,this.n);this.n+=c.length;
      while(this.pos+1<this.n){
        var i=this.pos|0,f=this.pos-i;
        if(i+1>=this.n)break;
        var s=this.buf[i]*(1-f)+this.buf[i+1]*f;
        this.pos+=this.step;
        this.acc+=s*s;this.cnt++;     // RMS 计量（放大前原始电平，判断采集端本身音量）
        if(this.cnt>=8000){var rms=Math.sqrt(this.acc/this.cnt);this.port.postMessage('rms'+Math.round(20*Math.log10(rms+1e-6)));this.acc=0;this.cnt=0;}
        s=s*MIC_GAIN;                 // iPad 原始电平偏低，软件放大
        s=0.6*Math.tanh(s/0.6);       // 软限幅：大音量压缩到±0.6，不硬削波（对讲清晰优先于保真）
        this.out[this.m++]=alaw(s);
        if(this.m===256){var b=this.out.slice(0).buffer;this.port.postMessage(b,[b]);this.m=0;}
      }
      var used=Math.min(this.pos,this.n)|0;   // pos 可能比 n 大 step-1，必须钳制，否则 n 变负数、下帧 set() 直接崩
      if(used>0){this.buf.copyWithin(0,used,this.n);this.n-=used;this.pos=Math.max(0,this.pos-used);}
    }
   }catch(e){
    if(!this.errSent){this.errSent=true;this.port.postMessage('WERR '+((e&&e.message)||e)+' @ '+((e&&e.stack)||'').split('\\n')[1]);}
   }
    return true;
  }
}
registerProcessor('ptt',P);
`;

// 页面加载就创建 AudioContext 并预载 Worklet 模块。
// iOS Safari 的坑：AudioContext 必须在"页面加载"或"用户手势"里创建，
// 若在按住按钮回调里 await 完麦克风权限再创建，用户激活已过期，
// resume() 静默失败、音频图不运转——表现是按钮变红但一个包都发不出去（实测）。
(async function initAudio(){
  try{
    ctx=new (window.AudioContext||window.webkitAudioContext)();
    var url=URL.createObjectURL(new Blob([WORKLET],{type:'application/javascript'}));
    await ctx.audioWorklet.addModule(url);
    workletReady=true;
  }catch(e){setSt('音频初始化失败：'+e.message);}
})();

async function ensureMic(){
  if(node)return true;
  if(!navigator.mediaDevices||!navigator.mediaDevices.getUserMedia){
    setSt('需要 HTTPS 才能用麦克风（请用 https:// 打开本页）');return false;}
  if(!workletReady){setSt('音频模块未就绪，请刷新页面');return false;}
  var stream;
  try{
    stream=await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:false,noiseSuppression:false,autoGainControl:false,channelCount:1}});
  }catch(e){setSt('麦克风被拒：'+e.message);return false;}
  try{
    var track=stream.getAudioTracks()[0];
    micTrack=track;
    if(track){
      track.onmute=function(){setSt('麦克风轨道被系统静音');report('TRACK_MUTED');};
      track.onunmute=function(){setSt(stText());report('TRACK_UNMUTED');};
      track.onended=function(){setSt('麦克风轨道已结束');report('TRACK_ENDED');};
    }
    var src=ctx.createMediaStreamSource(stream);
    node=new AudioWorkletNode(ctx,'ptt');
    // worklet 线程崩溃不会触发 window.onerror，必须挂这个事件才能看到
    node.onprocessorerror=function(e){setSt('编码器崩溃');report('PROC_ERROR');};
    sentN=0;encN=0;hbN=0;
    node.port.onmessage=function(ev){
      if(typeof ev.data==='string'&&ev.data.slice(0,2)==='hb'){
        hbN=parseInt(ev.data.slice(2),10);setSt(stText());return;
      }
      if(typeof ev.data==='string'&&ev.data.slice(0,4)==='WERR'){
        setSt('编码器错误：'+ev.data.slice(5));report(ev.data.slice(0,180));return;
      }
      if(typeof ev.data==='string'&&ev.data.slice(0,3)==='rms'){
        lastRms=parseInt(ev.data.slice(3),10);return;
      }
      encN++;
      if(talking&&ws&&ws.readyState===1){ws.send(ev.data);sentN++;}
      setSt(stText());   // 每个包都刷新，杜绝"数字没刷出来"的误判
    };
    // Worklet 必须接到 destination 才会被渲染引擎拉动。增益的两个坑都实测过：
    // · 精确的 0：WebKit 把链路判为"静音分支"裁剪，按住后只发 1 包就停；
    // · 0.0002（-74dB 听不见）：仍不拉动——iOS 对"基本不可闻"的链路会
    //   停止麦克风采集（防偷录的静音采集优化），引擎跳/编码全 0。
    // 用 0.03（-30dB 轻微耳返）：按住时门口机声音已静音（半双工），不会啸叫。
    gain=ctx.createGain();gain.gain.value=0.03;
    src.connect(node);node.connect(gain);gain.connect(ctx.destination);
  }catch(e){setSt('音频初始化失败：'+e.message);return false;}
  return true;
}

function muteVideo(m){
  try{
    var d=document.getElementById('vf').contentWindow.document;
    var v=d.querySelector('video');if(v)v.muted=m;
  }catch(e){}
}

// --- 下行放大器：门口机声音偏小，把播放器音频接进 Web Audio 增益（×AMP_GAIN）---
var AMP_GAIN=4, ampCtx=null;
function ampVideo(){
  try{
    var d=document.getElementById('vf').contentWindow.document;
    var v=d.querySelector('video');
    if(!v||v._amped)return;
    if(!ampCtx)ampCtx=new (window.AudioContext||window.webkitAudioContext)();
    var src=ampCtx.createMediaElementSource(v);
    var g=ampCtx.createGain();g.gain.value=AMP_GAIN;
    src.connect(g);g.connect(ampCtx.destination);
    v._amped=true;
    report('AMP_OK g='+AMP_GAIN);
  }catch(e){report('AMP_FAIL '+e.message);}
}
setInterval(ampVideo,2000);   // go2rtc 重建 video 元素后自动重新挂载
// iOS 要求 AudioContext 在用户手势里 resume：页面上任意一次触摸都尝试唤醒
document.addEventListener('pointerdown',function(){
  try{if(ampCtx&&ampCtx.state==='suspended')ampCtx.resume();}catch(e){}
});

async function press(ev){
  if(ev)ev.preventDefault();
  if(talking||busy)return;busy=true;
  // 1) 在用户手势里同步唤醒音频上下文（iOS 硬性要求，必须在手势内调用）
  if(ctx&&ctx.state==='suspended'){try{ctx.resume();}catch(e){}}
  // 2) 拿麦克风权限（首次会弹系统授权框，会消耗掉本次手势激活）
  if(!await ensureMic()){busy=false;return;}
  // 3) 权限弹窗后再补一次 resume；若仍未运行就明牌报错，别装没事
  if(ctx.state!=='running'){
    try{await ctx.resume();}catch(e){}
  }
  if(ctx.state!=='running'){
    setSt('音频未启动（'+ctx.state+'）：请松开按钮，再按一次');
    busy=false;return;
  }
  talking=true;btn.classList.add('on');btn.textContent='正在讲…';setSt(stText());
  report('PRESS ctx='+ctx.state+' track='+(micTrack?micTrack.readyState+',muted='+micTrack.muted+',enabled='+micTrack.enabled:'none'));
  tickTimer=setInterval(function(){report('TICK hb='+Math.floor(hbN/256)+' enc='+encN+' sent='+sentN+' rms='+lastRms+'dB ctx='+(ctx?ctx.state:'?'));},1000);
  muteVideo(true);    // 半双工消回声：按住时静音门口机声音，避免话筒回采
  busy=false;
}
function release(){
  if(!talking)return;talking=false;
  if(tickTimer){clearInterval(tickTimer);tickTimer=null;}
  btn.classList.remove('on');btn.textContent='按住 说话';setSt(wsState());
  if(ws&&ws.readyState===1){var sil=new Uint8Array(256).fill(0xD5);for(var i=0;i<3;i++)ws.send(sil);}
  try{if(ctx&&ctx.state==='running')ctx.suspend();}catch(e){}   // 松开即挂起音频图，停止采集省电
  muteVideo(false);
}
btn.addEventListener('pointerdown',press);
window.addEventListener('pointerup',release);
window.addEventListener('pointercancel',release);
btn.addEventListener('contextmenu',function(e){e.preventDefault();});

// --- 开门解锁 / 挂断通话：文本指令经 ws 发给服务器，服务器调 HA webhook 执行 ---
function sendCmd(c,tip){
  try{
    if(ws&&ws.readyState===1){ws.send(c);setSt(tip+'指令已发送');report('CMD '+c);}
    else setSt('连接未就绪，请稍后再点');
  }catch(e){setSt('发送失败：'+e.message);}
}
document.getElementById('btnUnlock').addEventListener('click',function(){sendCmd('CMD_UNLOCK','解锁');});
document.getElementById('btnHangup').addEventListener('click',function(){sendCmd('CMD_HANGUP','挂断');});
</script>
</body>
</html>"""

_panel_session = None


async def _get_session():
    global _panel_session
    if _panel_session is None or _panel_session.closed:
        _panel_session = ClientSession()
    return _panel_session


async def panel_index(request):
    return web.Response(text=INTERCOM_HTML, content_type="text/html",
                        headers={"Cache-Control": "no-store"})


async def ca_download(request):
    """下载自签 CA 根证书（新设备装信任用，和 iPad 描述文件同一个证书）。"""
    p = os.path.join(CERT_DIR, "ca.crt")
    if not os.path.exists(p):
        return web.Response(status=404, text="CA 证书不存在：" + p)
    data = open(p, "rb").read()
    return web.Response(body=data, content_type="application/x-x509-ca-cert",
                        headers={"Content-Disposition": 'attachment; filename="doorbell-ca.crt"'})


# 面板"开门解锁 / 挂断通话"按钮 → HA webhook → 按下对应的 button 实体。
# webhook_id 即密钥（随机串），仅家庭内网可达；链路：面板 ws → 本程序 → HA → ESP32 固件。
HA_BASE = os.environ.get("HA_BASE", "http://192.168.1.11:8123")
HA_CMD_WEBHOOK = {
    # 在 HA 里建两个 webhook 自动化触发 button.press（README「HA 自动化」一节），
    # 把 webhook_id 通过环境变量传进来；留空则面板上的解锁/挂断按钮不可用（其余功能正常）。
    "CMD_UNLOCK": os.environ.get("HA_WEBHOOK_UNLOCK", ""),
    "CMD_HANGUP": os.environ.get("HA_WEBHOOK_HANGUP", ""),
}

async def _ha_cmd(cmd):
    if not HA_CMD_WEBHOOK.get(cmd):
        log(f"面板指令 {cmd} 未配置 webhook（HA_WEBHOOK_* 环境变量为空），已忽略")
        return
    try:
        sess = await _get_session()
        async with sess.post(f"{HA_BASE}/api/webhook/{HA_CMD_WEBHOOK[cmd]}", timeout=10) as r:
            log(f"面板指令 {cmd} 已执行（HA 返回 {r.status}）")
    except Exception as e:
        log(f"面板指令 {cmd} 调用 HA 失败：{e}")


async def ws_talk(request):
    """上行音频通道：浏览器 WebSocket（二进制 A-law 包）→ UDP → ESP32 → 门口机。
    ESP32 固件已做双重闸口：只收桥接器 IP、且仅在通话/监视状态才转发给门口机。"""
    ws = web.WebSocketResponse(heartbeat=30)
    await ws.prepare(request)
    log(f"对讲面板已连接：{request.remote}")
    up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dst = (ESP32_IP, ESP32_AUDIO_PORT)
    n = 0
    # --- 上行整形队列：安卓 Chrome 的 WebSocket 会把小音频包粘成突发到达，门口机
    # 抖动缓冲兜不住就断续。这里按 32ms/包（256B@8kHz）的实时节奏转发，吸收突发。
    q = collections.deque()
    arr_stats = collections.deque(maxlen=500)   # 到达间隔统计（诊断用）
    last_arr = 0.0
    fwd_done = False

    async def forwarder():
        nonlocal n, fwd_done
        last_send = 0.0
        while not fwd_done:
            # 4 包（约128ms）抖动缓冲：起步先攒 4 包再开播，吸收平板→AP 的 WiFi 抖动
            if len(q) < 4:
                if not q:
                    last_send = 0.0
                await asyncio.sleep(0.004)
                continue
            now = time.monotonic()
            if last_send:
                wait = 0.032 - (now - last_send)
                if wait > 0:
                    await asyncio.sleep(wait)
            if len(q) > 16:   # 队列异常堆积（网络大乱）丢最旧的，防延迟越积越大
                q.popleft()
            data = q.popleft()
            up.sendto(data, dst)
            last_send = time.monotonic()
            n += 1
            if n == 1 or n % 100 == 0:
                log(f"上行音频转发中：已转 {n} 包（{request.remote}）")

    fwd_task = asyncio.ensure_future(forwarder())
    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.TEXT:
                text = str(msg.data)
                if text == "CMD_UNLOCK" or text == "CMD_HANGUP":
                    asyncio.ensure_future(_ha_cmd(text))
                else:
                    log(f"面板消息：{text[:200]}")
                continue
            if msg.type == web.WSMsgType.BINARY and msg.data:
                now = time.monotonic()
                if last_arr:
                    arr_stats.append(now - last_arr)
                last_arr = now
                q.append(msg.data)
                if len(arr_stats) == 500:
                    iv = list(arr_stats)
                    arr_stats.clear()
                    log(f"上行到达间隔 ms：min={min(iv)*1000:.0f} avg={sum(iv)/len(iv)*1000:.0f} "
                        f"max={max(iv)*1000:.0f} >60ms 的有 {sum(1 for x in iv if x > 0.06)} 个")
    finally:
        fwd_done = True
        fwd_task.cancel()
        try:
            await fwd_task
        except Exception:
            pass
        if n:
            log(f"对讲面板断开：{request.remote} code={ws.close_code}（本次共转发上行音频 {n} 包）")
        else:
            log(f"对讲面板断开：{request.remote} code={ws.close_code}")
    return ws


async def ws_proxy(request):
    """go2rtc 的 WebSocket 信令（/api/ws）原样代理到本机 go2rtc。"""
    ws_browser = web.WebSocketResponse(heartbeat=30)
    await ws_browser.prepare(request)
    sess = await _get_session()
    try:
        async with sess.ws_connect(f"ws://{GO2RTC_HTTP}{request.rel_url.path_qs}", heartbeat=30) as ws_go:
            async def browser_to_go():
                async for m in ws_browser:
                    if m.type == web.WSMsgType.TEXT:
                        await ws_go.send_str(m.data)
                    elif m.type == web.WSMsgType.BINARY:
                        await ws_go.send_bytes(m.data)
                    else:
                        break

            async def go_to_browser():
                async for m in ws_go:
                    if m.type == web.WSMsgType.TEXT:
                        await ws_browser.send_str(m.data)
                    elif m.type == web.WSMsgType.BINARY:
                        await ws_browser.send_bytes(m.data)
                    else:
                        break

            t1 = asyncio.ensure_future(browser_to_go())
            t2 = asyncio.ensure_future(go_to_browser())
            done, pending = await asyncio.wait({t1, t2}, return_when=asyncio.FIRST_COMPLETED)
            for t in pending:
                t.cancel()
    except Exception as e:
        log(f"代理 go2rtc WebSocket 失败：{e}")
    try:
        await ws_browser.close()
    except Exception:
        pass
    return ws_browser


_PROXY_SKIP = {"host", "connection", "content-length", "transfer-encoding",
               "upgrade", "keep-alive", "te", "trailer", "content-encoding"}


async def http_proxy(request):
    """其余路径（stream.html / main.js / api/...）反向代理到本机 go2rtc。
    目的：让面板里的画面与页面同源——既避免 https 页面嵌 http 视频流的混合内容拦截，
    又能在按住说话时直接跨 iframe 静音视频（半双工消回声）。"""
    sess = await _get_session()
    headers = {k: v for k, v in request.headers.items() if k.lower() not in _PROXY_SKIP}
    try:
        data = await request.read() if request.can_read_body else None
        async with sess.request(request.method, f"http://{GO2RTC_HTTP}{request.rel_url.path_qs}",
                                headers=headers, data=data, allow_redirects=False) as resp:
            body = await resp.read()
            out_headers = {k: v for k, v in resp.headers.items() if k.lower() not in _PROXY_SKIP}
            out_headers["Cache-Control"] = "no-store"
            return web.Response(status=resp.status, headers=out_headers, body=body)
    except Exception as e:
        return web.Response(status=502, text=f"go2rtc 代理失败：{e}")


def panel_thread():
    """对讲面板 HTTP(S) 服务。有证书用 HTTPS（浏览器才给麦克风权限），否则退回 HTTP 并提示。"""
    app = web.Application()
    app.router.add_get("/", panel_index)
    app.router.add_get("/ws/talk", ws_talk)
    app.router.add_get("/api/ws", ws_proxy)
    app.router.add_get("/ca.crt", ca_download)
    app.router.add_route("*", "/{path:.*}", http_proxy)
    ssl_ctx = None
    crt = os.path.join(CERT_DIR, "server.crt")
    key = os.path.join(CERT_DIR, "server.key")
    if os.path.exists(crt) and os.path.exists(key):
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(crt, key)
        log(f"对讲面板（HTTPS）已就绪，端口 {PANEL_PORT}。"
            "iPad 首次使用需安装根证书（见说明文档「对讲」一章）")
    else:
        log(f"未找到 {crt}，对讲面板退回 HTTP（端口 {PANEL_PORT}）。"
            "注意：HTTP 下浏览器不给麦克风权限，只能看不能讲——请运行 gen_certs.sh 生成证书后重启容器")
    web.run_app(app, host="0.0.0.0", port=PANEL_PORT, ssl_context=ssl_ctx,
                print=None, handle_signals=False)


if __name__ == "__main__":
    main()

