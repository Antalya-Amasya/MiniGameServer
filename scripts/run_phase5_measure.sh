#!/bin/bash
# Release-server measurements. Uses minigame_test only. Does not print secrets.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build-linux-release/bin/minigame_server"
PY="$ROOT/scripts/stress_server.py"
export MINIGAME_DB_NAME=minigame_test
export MINIGAME_TEST_DB_NAME=minigame_test

start_server() {
  local idle="${1:-}"
  : > /tmp/mg-server.log
  if [[ -n "$idle" ]]; then
    "$BIN" --port 0 --idle-ms "$idle" >/tmp/mg-server.log 2>&1 &
  else
    "$BIN" --port 0 >/tmp/mg-server.log 2>&1 &
  fi
  echo $! >/tmp/mg-server.pid
  for _ in $(seq 1 50); do
    if grep -q "listening on port" /tmp/mg-server.log; then
      grep -o "listening on port [0-9]*" /tmp/mg-server.log | awk '{print $4}' | tail -1
      return
    fi
    sleep 0.1
  done
  echo "server did not start" >&2
  cat /tmp/mg-server.log >&2
  exit 1
}

stop_server() {
  local pid
  pid="$(cat /tmp/mg-server.pid)"
  local start end
  start="$(date +%s%3N)"
  kill -TERM "$pid" 2>/dev/null || true
  for _ in $(seq 1 200); do
    if ! kill -0 "$pid" 2>/dev/null; then
      end="$(date +%s%3N)"
      echo "SHUTDOWN_MS $((end - start))"
      return
    fi
    sleep 0.05
  done
  echo "SHUTDOWN_TIMEOUT"
  kill -KILL "$pid" 2>/dev/null || true
}

rss_of() {
  awk '/VmRSS/ {print $2}' "/proc/$1/status"
}

fds_of() {
  find "/proc/$1/fd" -mindepth 1 | wc -l
}

PORT="$(start_server)"
PID="$(cat /tmp/mg-server.pid)"
echo "SERVER_PORT $PORT"
echo "RSS_BASELINE_KB $(rss_of "$PID")"
echo "FD_BASELINE $(fds_of "$PID")"
echo "DB_BEFORE $(bash "$ROOT/scripts/count_app_sessions.sh")"

python3 "$PY" --host 127.0.0.1 --port "$PORT" --scenario connect --clients 1 --duration 1 --warmup 0 >/dev/null
echo "FD_AFTER_ONE $(fds_of "$PID")"

python3 - <<PY
import socket
port = int("$PORT")
for _ in range(1000):
    client = socket.create_connection(("127.0.0.1", port), 2)
    client.sendall(b"PING\n")
    client.close()
print("FD_CYCLE 1000")
PY
sleep 0.5
echo "FD_AFTER_1000 $(fds_of "$PID")"
echo "RSS_AFTER_FD_KB $(rss_of "$PID")"
echo "DB_DURING $(bash "$ROOT/scripts/count_app_sessions.sh")"

run_case() {
  python3 "$PY" --host 127.0.0.1 --port "$PORT" --scenario "$1" --clients "$2" --duration 10 --warmup 2
}

for clients in 10 50 100; do
  run_case connect "$clients"
done
for clients in 10 50; do
  run_case login "$clients"
  run_case ping "$clients"
  run_case room "$clients"
  run_case database "$clients"
done
run_case game 10
run_case login 100 || true
run_case database 100 || true

echo "RSS_DURING_KB $(rss_of "$PID")"
python3 "$PY" --host 127.0.0.1 --port "$PORT" --scenario abuse --clients 1 --duration 1 --warmup 0
python3 "$PY" --host 127.0.0.1 --port "$PORT" --scenario backpressure --clients 1 --duration 1 --warmup 0
python3 "$PY" --host 127.0.0.1 --port "$PORT" --scenario slow --clients 1 --duration 1 --warmup 0
echo "RSS_AFTER_ABUSE_KB $(rss_of "$PID")"
echo "FD_AFTER_STRESS $(fds_of "$PID")"

python3 "$PY" --host 127.0.0.1 --port "$PORT" --scenario ping --clients 20 --duration 20 --warmup 2 &
LOAD_PID=$!
sleep 2
if command -v perf >/dev/null 2>&1; then
  if perf record -p "$PID" -o /tmp/mg-perf.data -- sleep 8 >/tmp/mg-perf-record.txt 2>&1; then
    echo "PERF_RECORD_OK"
    perf report -i /tmp/mg-perf.data --stdio --percent-limit 2 | head -n 40
  else
    echo "PERF_NOT_AVAILABLE_IN_CURRENT_WSL"
    cat /tmp/mg-perf-record.txt
  fi
else
  echo "PERF_NOT_AVAILABLE_IN_CURRENT_WSL"
fi
wait "$LOAD_PID" || true

echo "RSS_BEFORE_SHUTDOWN_KB $(rss_of "$PID")"
stop_server
sleep 0.5
echo "DB_AFTER $(bash "$ROOT/scripts/count_app_sessions.sh")"

PORT="$(start_server 1000)"
PID="$(cat /tmp/mg-server.pid)"
python3 - <<PY
import socket, time
port = int("$PORT")
client = socket.create_connection(("127.0.0.1", port), 2)
time.sleep(2.5)
client.settimeout(1)
try:
    data = client.recv(100)
    print("IDLE_RESULT", "CLOSED" if data == b"" else data[:40])
except Exception as ex:
    print("IDLE_RESULT", type(ex).__name__)
finally:
    client.close()
PY
echo "FD_IDLE_SERVER $(fds_of "$PID")"
stop_server
echo "MEASURE_DONE"
