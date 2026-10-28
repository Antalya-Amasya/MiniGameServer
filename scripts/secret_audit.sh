#!/bin/bash
set -u
cd "$(dirname "$0")/.."
git check-ignore -v .env.local sql/provision_project_user.local.sql || true
echo "STATUS"
git status --short -- .env.local sql/provision_project_user.local.sql sql/provision_project_user.sql
echo "TRACKED_SQL"
git ls-files "sql/*.sql" ".env*" || true
echo "PLACEHOLDER"
grep -n "REPLACE_WITH_A_LONG_RANDOM_PASSWORD" sql/provision_project_user.sql || true
echo "AUDIT_DONE"
