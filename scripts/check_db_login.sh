#!/bin/bash
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
set -a
# shellcheck disable=SC1091
. "$ROOT/.env.local"
set +a
check() {
  local db="$1"
  if mysql --protocol=TCP --host="$MINIGAME_DB_HOST" --port="$MINIGAME_DB_PORT" \
      --user="$MINIGAME_DB_USER" --password="$MINIGAME_DB_PASSWORD" \
      --batch --skip-column-names "$db" -e "SELECT 1;" >/tmp/mg-db-out.txt 2>/tmp/mg-db-err.txt; then
    if grep -q '^1$' /tmp/mg-db-out.txt; then
      echo "$db PASS"
    else
      echo "$db EMPTY"
    fi
  else
    sanitized=$(tr '\n' ' ' < /tmp/mg-db-err.txt | sed "s/${MINIGAME_DB_PASSWORD}/<redacted>/g")
    echo "$db FAIL $sanitized"
  fi
  rm -f /tmp/mg-db-out.txt /tmp/mg-db-err.txt
}
check "$MINIGAME_DB_NAME"
check "$MINIGAME_TEST_DB_NAME"
