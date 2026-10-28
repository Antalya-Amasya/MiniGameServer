#!/usr/bin/env bash
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build-linux-baseline/bin"
PORT=19123
"$BIN/minigame_server" "$PORT" > /tmp/mgs-base-server.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; wait $PID 2>/dev/null || true' EXIT
for i in $(seq 1 50); do
  if grep -q "listening on port $PORT" /tmp/mgs-base-server.log 2>/dev/null; then
    break
  fi
  sleep 0.1
done
grep -q "listening on port $PORT" /tmp/mgs-base-server.log
python3 - <<'PY'
import socket
s = socket.create_connection(("127.0.0.1", 19123), timeout=3)
s.sendall(b"LOGIN alice\n")
data = b""
while b"\n" not in data:
    chunk = s.recv(256)
    if not chunk:
        break
    data += chunk
s.sendall(b"QUIT\n")
s.close()
text = data.decode()
print(text, end="")
if "OK LOGIN alice" not in text:
    raise SystemExit("missing login ack")
print("BASELINE_CLIENT_OK")
PY
if ! kill -0 "$PID" 2>/dev/null; then
  echo "server died"
  exit 1
fi
echo "BASELINE_SERVER_STILL_UP"
