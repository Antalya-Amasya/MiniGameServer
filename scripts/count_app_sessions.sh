#!/bin/bash
# Prints how many sessions this app account can see. Does not print the password.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
set -a
# shellcheck disable=SC1091
source "$ROOT/.env.local"
set +a
MINIGAME_DB_HOST="${MINIGAME_DB_HOST//$'\r'/}"
MINIGAME_DB_PORT="${MINIGAME_DB_PORT//$'\r'/}"
MINIGAME_DB_USER="${MINIGAME_DB_USER//$'\r'/}"
MINIGAME_DB_PASSWORD="${MINIGAME_DB_PASSWORD//$'\r'/}"
cnf="$(mktemp)"
chmod 600 "$cnf"
cat >"$cnf" <<EOF
[client]
protocol=TCP
host=${MINIGAME_DB_HOST}
port=${MINIGAME_DB_PORT}
user=${MINIGAME_DB_USER}
password=${MINIGAME_DB_PASSWORD}
EOF
count="$(mysql --defaults-extra-file="$cnf" -N -e "SHOW PROCESSLIST" | wc -l)"
rm -f "$cnf"
echo "$count"
