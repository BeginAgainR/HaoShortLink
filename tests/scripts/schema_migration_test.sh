#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MYSQL_IMAGE="${SHORTLINK_MYSQL_IMAGE:-mysql:8.0}"
RUN_ID="$$-${RANDOM}"
MYSQL_CONTAINER="hao-shortlink-migration-test-${RUN_ID}"
MYSQL_NETWORK="hao-shortlink-migration-test-${RUN_ID}"
ROOT_PASSWORD="migration-root-password"
APP_PASSWORD="migration-app-password"
DATABASE="hao_shortlink_migration_test"

cleanup()
{
    docker rm -f "${MYSQL_CONTAINER}" >/dev/null 2>&1 || true
    docker network rm "${MYSQL_NETWORK}" >/dev/null 2>&1 || true
}

fail()
{
    echo "FAIL: $*" >&2
    docker logs --tail=120 "${MYSQL_CONTAINER}" >&2 || true
    exit 1
}

mysql_root()
{
    docker exec -i "${MYSQL_CONTAINER}" mysql \
        -uroot -p"${ROOT_PASSWORD}" "${DATABASE}" "$@"
}

mysql_value()
{
    mysql_root --batch --skip-column-names --execute="$1" 2>/dev/null
}

run_migration()
{
    docker run --rm \
        --network "${MYSQL_NETWORK}" \
        --entrypoint /bin/bash \
        -v "${ROOT_DIR}:/workspace:ro" \
        -w /workspace \
        -e HAOHTTP_MYSQL_HOST=mysql \
        -e HAOHTTP_MYSQL_PORT=3306 \
        -e HAOHTTP_MYSQL_USER=hao_shortlink \
        -e HAOHTTP_MYSQL_PASSWORD="${APP_PASSWORD}" \
        -e HAOHTTP_MYSQL_DATABASE="${DATABASE}" \
        "${MYSQL_IMAGE}" \
        tests/scripts/migrate_shortlink_schema.sh "$@"
}

expect_eq()
{
    local actual="$1"
    local expected="$2"
    local label="$3"
    [[ "${actual}" == "${expected}" ]] || \
        fail "${label}: expected ${expected}, got ${actual}"
}

trap cleanup EXIT

command -v docker >/dev/null 2>&1 || fail "docker is required"
docker info >/dev/null 2>&1 || fail "Docker is not available"

docker network create "${MYSQL_NETWORK}" >/dev/null
docker run -d \
    --name "${MYSQL_CONTAINER}" \
    --network "${MYSQL_NETWORK}" \
    --network-alias mysql \
    --tmpfs /var/lib/mysql \
    -e MYSQL_ROOT_PASSWORD="${ROOT_PASSWORD}" \
    -e MYSQL_DATABASE="${DATABASE}" \
    -e MYSQL_USER=hao_shortlink \
    -e MYSQL_PASSWORD="${APP_PASSWORD}" \
    "${MYSQL_IMAGE}" >/dev/null

for _ in {1..90}; do
    if docker exec "${MYSQL_CONTAINER}" mysqladmin ping \
        -h 127.0.0.1 -uroot -p"${ROOT_PASSWORD}" --silent >/dev/null 2>&1; then
        break
    fi
    sleep 1
done
docker exec "${MYSQL_CONTAINER}" mysqladmin ping \
    -h 127.0.0.1 -uroot -p"${ROOT_PASSWORD}" --silent >/dev/null 2>&1 || \
    fail "isolated MySQL did not become ready"

run_migration up
expect_eq "$(mysql_value "SELECT COUNT(*) FROM schema_migrations WHERE version BETWEEN 1 AND 5")" \
    "5" "empty database migration versions"
expect_eq "$(mysql_value "SELECT COUNT(*) FROM users")" "1" \
    "empty database legacy owner"
expect_eq "$(mysql_value "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name IN ('short_links', 'users', 'user_sessions', 'processed_access_events', 'short_link_access_totals', 'short_link_access_hourly')")" \
    "6" "empty database schema objects"
echo "PASS: empty database initializes through migration 005"

docker exec "${MYSQL_CONTAINER}" mysql -uroot -p"${ROOT_PASSWORD}" \
    --execute="DROP DATABASE \`${DATABASE}\`; CREATE DATABASE \`${DATABASE}\` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci; GRANT ALL PRIVILEGES ON \`${DATABASE}\`.* TO 'hao_shortlink'@'%';" \
    >/dev/null
for migration in "${ROOT_DIR}"/apps/shortlink_server/sql/00{1,2,3,4}_*.sql; do
    mysql_root < "${migration}"
done
mysql_root --execute="
    INSERT INTO short_links (code, original_url, status, expires_at)
    VALUES ('LegacyA', 'https://example.com/v1.9-history', 'disabled', '2030-01-02 03:04:05');
    SET @short_link_id = LAST_INSERT_ID();
    INSERT INTO short_link_access_totals
        (short_link_id, result, access_count, first_occurred_at_ms, last_occurred_at_ms)
    VALUES (@short_link_id, 'success', 7, 1784304000000, 1784307600000);
" >/dev/null

run_migration up
expect_eq "$(mysql_value "SELECT CONCAT(s.code, '|', s.original_url, '|', s.status, '|', DATE_FORMAT(s.expires_at, '%Y-%m-%d %H:%i:%s'), '|', u.username_normalized, '|', u.status) FROM short_links s JOIN users u ON u.id = s.owner_id WHERE s.code = 'LegacyA'")" \
    "LegacyA|https://example.com/v1.9-history|disabled|2030-01-02 03:04:05|legacy-system|disabled" \
    "v1.9 historical link owner backfill"
expect_eq "$(mysql_value "SELECT access_count FROM short_link_access_totals t JOIN short_links s ON s.id = t.short_link_id WHERE s.code = 'LegacyA' AND t.result = 'success'")" \
    "7" "v1.9 statistics preservation"
run_migration up
expect_eq "$(mysql_value "SELECT COUNT(*) FROM users WHERE username_normalized = 'legacy-system'")" \
    "1" "idempotent migration"
echo "PASS: v1.9 upgrade preserves lifecycle and statistics and is idempotent"

run_migration down --allow-data-loss
expect_eq "$(mysql_value "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'short_links' AND column_name = 'owner_id'")" \
    "0" "rollback owner column"
expect_eq "$(mysql_value "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name IN ('users', 'user_sessions')")" \
    "0" "rollback identity tables"
expect_eq "$(mysql_value "SELECT CONCAT(code, '|', original_url, '|', status, '|', DATE_FORMAT(expires_at, '%Y-%m-%d %H:%i:%s')) FROM short_links WHERE code = 'LegacyA'")" \
    "LegacyA|https://example.com/v1.9-history|disabled|2030-01-02 03:04:05" \
    "rollback v1.9 link preservation"
expect_eq "$(mysql_value "SELECT access_count FROM short_link_access_totals t JOIN short_links s ON s.id = t.short_link_id WHERE s.code = 'LegacyA' AND t.result = 'success'")" \
    "7" "rollback v1.9 statistics preservation"
echo "PASS: rollback removes only v2.0 identity and ownership objects"

run_migration up
expect_eq "$(mysql_value "SELECT COUNT(*) FROM schema_migrations WHERE version BETWEEN 1 AND 5")" \
    "5" "re-upgrade migration versions"
expect_eq "$(mysql_value "SELECT COUNT(*) FROM short_links s JOIN users u ON u.id = s.owner_id WHERE s.code = 'LegacyA' AND u.username_normalized = 'legacy-system'")" \
    "1" "re-upgrade owner backfill"
echo "PASS: migration 005 can be applied again after rollback"

echo "Schema migration test passed"
