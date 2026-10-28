#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
echo "PERF_BIN $(command -v perf || echo NO_PERF_BIN)"
if perf stat -e cycles -- true >/tmp/mg-perf-stat.txt 2>&1; then
  echo "PERF_STAT_OK"
else
  echo "PERF_RUNTIME_FAIL"
fi
tail -n 15 /tmp/mg-perf-stat.txt || true
echo "LOG_SECRET_HITS $(grep -E -c 'secret1|MINIGAME_DB_PASSWORD|\$2b\$' /tmp/mg-server.log || true)"
export MINIGAME_DB_NAME=minigame_test
BIN="$ROOT/build-linux-release/bin/minigame_server"
"$BIN" --port 0 >/tmp/mg-stable.log 2>&1 &
echo $! >/tmp/mg-stable.pid
port=""
for _ in $(seq 1 50); do
  port="$(grep -o 'listening on port [0-9]*' /tmp/mg-stable.log | awk '{print $4}' | tail -1 || true)"
  if [[ -n "$port" ]]; then
    break
  fi
  sleep 0.1
done
echo "STABLE_PORT $port"
pid="$(cat /tmp/mg-stable.pid)"
echo "RSS_STABLE_BEFORE $(awk '/VmRSS/ {print $2}' /proc/$pid/status)"
echo "FD_STABLE_BEFORE $(find /proc/$pid/fd -mindepth 1 | wc -l)"
python3 scripts/stress_server.py --host 127.0.0.1 --port "$port" --scenario ping --clients 20 --duration 120 --warmup 2
echo "RSS_STABLE_AFTER $(awk '/VmRSS/ {print $2}' /proc/$pid/status)"
echo "FD_STABLE_AFTER $(find /proc/$pid/fd -mindepth 1 | wc -l)"
python3 scripts/stress_server.py --host 127.0.0.1 --port "$port" --scenario database --clients 100 --duration 10 --warmup 1 --timeout 8 || echo "DATABASE_100_CLIENT_ERROR"
kill -TERM "$pid" || true
wait "$pid" || true
echo "DB_AFTER_STABLE $(bash scripts/count_app_sessions.sh)"
echo "FOLLOWUP_DONE"
