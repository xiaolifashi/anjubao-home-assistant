#!/bin/sh
# 门禁视频桥启动脚本（2026-10-31 修正版）
#
# 相对原版唯一的改动：给 ffmpeg 加 -max_interleave_delta 0
# 原因：监视时门口机只有视频、没有音频，桥仍在往 ffmpeg 喂静音包，
# 音频输入长期"饥饿"，复用器等不到音频就把 RTSP 连接拖断（Broken pipe），
# 表现为面板画面卡死。加这个参数后复用器不再为音频等待，连接保持稳定。
set -u

while true; do
  python3 -u /app/bridge.py | \
    ffmpeg -hide_banner -loglevel warning \
      -f h264 -framerate 25 -i pipe:0 \
      -thread_queue_size 512 \
      -f alaw -ar 8000 -ac 1 \
      -i "udp://127.0.0.1:9991?fifo_size=65536&overrun_nonfatal=1" \
      -map 0:v -map 1:a -c copy \
      -max_interleave_delta 0 \
      -f rtsp -rtsp_transport tcp "$RTSP_URL"
  echo '[video-bridge] ffmpeg 退出，3 秒后重连' >&2
  sleep 3
done
