#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SQL_DIR="${PROJECT_ROOT}/apps/shortlink_server/sql"

MYSQL_HOST="${HAOHTTP_MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${HAOHTTP_MYSQL_PORT:-3306}"
MYSQL_USER="${HAOHTTP_MYSQL_USER:-hao_shortlink}"
MYSQL_PASSWORD="${HAOHTTP_MYSQL_PASSWORD:-hao_shortlink}"
MYSQL_DATABASE="${HAOHTTP_MYSQL_DATABASE:-hao_shortlink}"

usage() {
    echo "Usage: $0 up | down --allow-data-loss" >&2
}

mysql_command() {
    MYSQL_PWD="${MYSQL_PASSWORD}" mysql \
        --protocol=TCP \
        --host="${MYSQL_HOST}" \
        --port="${MYSQL_PORT}" \
        --user="${MYSQL_USER}" \
        --database="${MYSQL_DATABASE}" \
        --batch --skip-column-names "$@"
}

table_exists() {
    local table_name="$1"
    [[ "$(mysql_command --execute="SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = '${table_name}'")" == "1" ]]
}

column_exists() {
    local table_name="$1"
    local column_name="$2"
    [[ "$(mysql_command --execute="SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = '${table_name}' AND column_name = '${column_name}'")" == "1" ]]
}

validate_v2_schema() {
    table_exists users || {
        echo "Schema migration 005 is marked applied but users is missing." >&2
        return 1
    }
    table_exists user_sessions || {
        echo "Schema migration 005 is marked applied but user_sessions is missing." >&2
        return 1
    }
    column_exists short_links owner_id || {
        echo "Schema migration 005 is marked applied but short_links.owner_id is missing." >&2
        return 1
    }

    [[ "$(mysql_command --execute="SELECT IS_NULLABLE FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'short_links' AND column_name = 'owner_id'")" == "NO" ]] || {
        echo "short_links.owner_id must be NOT NULL." >&2
        return 1
    }
    [[ "$(mysql_command --execute="SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema = DATABASE() AND table_name = 'short_links' AND index_name = 'idx_short_links_owner_id_id'")" -gt 0 ]] || {
        echo "Owner pagination index is missing." >&2
        return 1
    }
    [[ "$(mysql_command --execute="SELECT COUNT(*) FROM information_schema.referential_constraints WHERE constraint_schema = DATABASE() AND table_name = 'short_links' AND constraint_name = 'fk_short_links_owner'")" == "1" ]] || {
        echo "Short-link owner foreign key is missing." >&2
        return 1
    }
    [[ "$(mysql_command --execute="SELECT GROUP_CONCAT(CONCAT(version, ':', name) ORDER BY version SEPARATOR ',') FROM schema_migrations WHERE version BETWEEN 1 AND 5")" == "1:create_short_links,2:make_short_link_code_case_sensitive,3:add_short_link_lifecycle,4:create_access_statistics,5:add_users_sessions_and_ownership" ]] || {
        echo "Schema migration history must contain the expected versions 1 through 5." >&2
        return 1
    }
    [[ "$(mysql_command --execute="SELECT COUNT(*) FROM users WHERE username_normalized = 'legacy-system' AND status = 'disabled' AND password_hash = '!'")" == "1" ]] || {
        echo "Disabled legacy-system owner is missing." >&2
        return 1
    }
    [[ "$(mysql_command --execute="SELECT COUNT(*) FROM short_links WHERE owner_id IS NULL")" == "0" ]] || {
        echo "Some short links do not have an owner." >&2
        return 1
    }
}

apply_up() {
    mysql_command --execute="CREATE TABLE IF NOT EXISTS schema_migrations (version INT UNSIGNED NOT NULL, name VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, applied_at TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3), PRIMARY KEY (version)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"

    if [[ "$(mysql_command --execute="SELECT COUNT(*) FROM schema_migrations WHERE version = 5")" == "1" ]]; then
        validate_v2_schema
        echo "Schema is already at v2.0 migration 005."
        return
    fi

    if ! table_exists short_links; then
        mysql_command < "${SQL_DIR}/001_create_short_links.sql"
        mysql_command < "${SQL_DIR}/002_make_short_link_code_case_sensitive.sql"
        mysql_command < "${SQL_DIR}/003_add_short_link_lifecycle.sql"
        mysql_command < "${SQL_DIR}/004_create_access_statistics.sql"
    elif ! column_exists short_links status || ! table_exists processed_access_events; then
        echo "Existing database is not an empty schema or supported v1.9 schema." >&2
        exit 1
    fi

    mysql_command < "${SQL_DIR}/005_add_users_sessions_and_ownership.sql"
    validate_v2_schema
    echo "Applied schema migration 005."
}

apply_down() {
    if [[ "${2:-}" != "--allow-data-loss" ]]; then
        echo "Rollback drops v2.0 users and sessions. Pass --allow-data-loss explicitly." >&2
        exit 1
    fi
    if ! table_exists schema_migrations || \
       [[ "$(mysql_command --execute="SELECT COUNT(*) FROM schema_migrations WHERE version = 5")" != "1" ]]; then
        echo "Schema migration 005 is not applied."
        return
    fi
    mysql_command < "${SQL_DIR}/rollback/005_remove_users_sessions_and_ownership.sql"
    echo "Rolled back schema migration 005."
}

case "${1:-}" in
    up)
        apply_up
        ;;
    down)
        apply_down "$@"
        ;;
    *)
        usage
        exit 2
        ;;
esac
