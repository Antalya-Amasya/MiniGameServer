#!/bin/bash
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export MINIGAME_DB_NAME=minigame_test
BIN="$ROOT/build-linux-release/bin/minigame_server"
"$BIN" --port 0 >/tmp/mg-shutdown.log 2>&1 &
echo $! >/tmp/mg-shutdown.pid
port=""
for _ in $(seq 1 50); do
  port="$(grep -o 'listening on port [0-9]*' /tmp/mg-shutdown.log | awk '{print $4}' | tail -1 || true)"
  if [[ -n "$port" ]]; then
    break
  fi
  sleep 0.1
done
python3 "$ROOT/scripts/stress_server.py" --host 127.0.0.1 --port "$port" --scenario ping --clients 20 --duration 12 --warmup 0 >/tmp/mg-shutdown-client.log 2>&1 &
sleep 3
pid="$(cat /tmp/mg-shutdown.pid)"
start="$(date +%s%3N)"
kill -TERM "$pid" || true
for _ in $(seq 1 200); do
  if ! kill -0 "$pid" 2>/dev/null; then
    end="$(date +%s%3N)"
    echo "SHUTDOWN_UNDER_LOAD_MS $((end - start))"
    echo "SHUTDOWN_UNDER_LOAD_DONE"
    exit 0
  fi
  sleep 0.05
done
echo "SHUTDOWN_UNDER_LOAD_TIMEOUT"
exit 1
