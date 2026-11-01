# -*- coding: utf-8 -*-
"""上传修改后的 bridge.py 到 NAS 并覆盖（上传前 NAS 上请自行备份旧版）。

用法:
  set NAS_HOST=192.168.1.10
  set NAS_USER=1
  set NAS_PASSWORD=你的密码
  python upload_bridge.py
"""
import os, paramiko

HOST = os.environ.get("NAS_HOST", "192.168.1.10")
USER = os.environ.get("NAS_USER", "1")
PWD  = os.environ.get("NAS_PASSWORD", "changeme")

LOCAL  = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bridge", "bridge.py")
REMOTE = "/vol1/1000/docker/doorbell-video/bridge.py"   # 按你的实际部署路径修改

t = paramiko.Transport((HOST, 22))
t.connect(username=USER, password=PWD)
sftp = paramiko.SFTPClient.from_transport(t)
sftp.put(LOCAL, REMOTE)
sftp.close(); t.close()
print("uploaded:", REMOTE)
