#!/bin/bash
set -eu
echo "=== route ==="
ip route | head -n 3
echo "=== nameserver ==="
awk '/^nameserver/{print $2; exit}' /etc/resolv.conf
echo "=== packages ==="
dpkg -l | grep -E 'libmysqlclient|mysql-client|libmariadb' || true
echo "=== headers ==="
ls /usr/include/mysql/mysql.h 2>/dev/null || true
ls /usr/include/mariadb/mysql.h 2>/dev/null || true
