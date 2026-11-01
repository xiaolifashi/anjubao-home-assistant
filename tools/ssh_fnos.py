# -*- coding: utf-8 -*-
"""SSH 到 NAS（fnOS），执行命令并打印输出。

用法:
  set NAS_HOST=192.168.1.10
  set NAS_USER=1
  set NAS_PASSWORD=你的密码
  python ssh_fnos.py "docker ps" [timeout秒]
"""
import os, sys, paramiko

HOST = os.environ.get("NAS_HOST", "192.168.1.10")
USER = os.environ.get("NAS_USER", "1")
PWD  = os.environ.get("NAS_PASSWORD", "changeme")

def run(cmd, timeout=30):
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, 22, USER, PWD, timeout=15, banner_timeout=15, auth_timeout=15)
    stdin, stdout, stderr = c.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode("utf-8", "replace")
    err = stderr.read().decode("utf-8", "replace")
    c.close()
    return out, err

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "id; uname -a; whoami"
    try:
        out, err = run(cmd, timeout=int(sys.argv[2]) if len(sys.argv) > 2 else 30)
        if out: print(out)
        if err: print("STDERR:", err)
    except Exception as e:
        print("ERROR:", e)
        sys.exit(1)
