#!/bin/sh
# 生成对讲面板 HTTPS 用的本地 CA + 服务器证书（自签名，只在你家局域网用）。
# 用法：在 doorbell-video 目录里执行  sh gen_certs.sh
# 然后把 certs/ca.crt 装到 iPad 并信任（步骤见《门禁视频接入说明》对讲一章），
# 最后重启 video-bridge 容器。
set -e
cd "$(dirname "$0")"
mkdir -p certs

SERVER_IP="192.168.1.10"   # ← 改成你的 NAS（跑视频桥的机器）IP   # 飞牛 NAS 的 IP；变了就改这里再重跑

if [ ! -f certs/ca.key ]; then
  echo "== 生成根证书 CA（10 年有效，只需装一次到 iPad）"
  openssl genrsa -out certs/ca.key 4096
  openssl req -x509 -new -nodes -key certs/ca.key -sha256 -days 3650 \
    -out certs/ca.crt -subj "/CN=Doorbell Local CA"
else
  echo "== 已存在 CA，跳过（保留 iPad 上已装的根证书有效）"
fi

cat > certs/server.cnf <<EOF
[req]
distinguished_name = dn
req_extensions = v3_req
[dn]
[v3_req]
subjectAltName = IP:${SERVER_IP}
EOF

echo "== 生成服务器证书（有效期 825 天，苹果设备允许的上限内）"
openssl genrsa -out certs/server.key 2048
openssl req -new -key certs/server.key -out certs/server.csr \
  -subj "/CN=doorbell-panel"
openssl x509 -req -in certs/server.csr -CA certs/ca.crt -CAkey certs/ca.key \
  -CAcreateserial -out certs/server.crt -days 825 -sha256 \
  -extfile certs/server.cnf -extensions v3_req
rm -f certs/server.csr

echo ""
echo "完成！接下来："
echo "  1. 把 certs/ca.crt 发到 iPad 并安装（见说明文档）"
echo "  2. 重启容器：docker compose restart video-bridge（或在 fnOS 面板里重启）"
echo "  3. iPad Safari 打开 https://${SERVER_IP}:8443/"