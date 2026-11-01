FROM alpine:3.20
RUN apk add --no-cache python3 py3-pip ffmpeg \
 && pip install --no-cache-dir --break-system-packages aiohttp
COPY bridge.py /app/bridge.py

# go2rtc 的 RTSP 收流地址（host 网络模式下就是本机回环）
ENV RTSP_URL=rtsp://127.0.0.1:8554/menjin

# 视频：裸 H.264 字节流(stdin) ──╮
# 音频：裸 A-law 8kHz(UDP 9991) ──┼─> ffmpeg 封装 -> RTSP(视频 H.264 + 音频 PCMA) 推给 go2rtc。
# ffmpeg 断开（go2rtc 重启/网络抖动）时 3 秒后自动重连。
# 注意（沙箱实测踩坑）：
#   · 裸 h264 输入绝不能加 -fflags nobuffer —— 会把帧全部吃掉，一路黑屏（ffmpeg 5.1/7.0 均复现）；
#   · 两路输入都不加 use_wallclock，统一"计数时间戳"，音视频才能对齐；
#   · 音频 UDP 输入必须带 fifo_size（启用内置环形缓冲线程），否则双输入时音频读两包就停摆；
#   · bridge.py 侧会按视频节奏补静音包，保证 ffmpeg 探测和混流随时有音频可发。
CMD ["sh", "-c", "while true; do \
      python3 -u /app/bridge.py | \
      ffmpeg -hide_banner -loglevel warning \
        -f h264 -framerate 25 -i pipe:0 \
        -thread_queue_size 512 \
        -f alaw -ar 8000 -ac 1 \
        -i \"udp://127.0.0.1:9991?fifo_size=65536&overrun_nonfatal=1\" \
        -map 0:v -map 1:a -c copy -f rtsp -rtsp_transport tcp \"$RTSP_URL\" && break; \
      echo '[video-bridge] ffmpeg 退出，3 秒后重连' >&2; sleep 3; \
    done"]
