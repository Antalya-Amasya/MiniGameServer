#!/bin/bash
python3 - <<'PY'
import os
import socket
s = socket.socket()
s.settimeout(3)
try:
    host = os.environ.get("MINIGAME_DB_HOST", "127.0.0.1")
    port = int(os.environ.get("MINIGAME_DB_PORT", "3306"))
    s.connect((host, port))
    print("PORT_OPEN")
except Exception as ex:
    print("PORT_FAIL", type(ex).__name__)
finally:
    s.close()
PY
