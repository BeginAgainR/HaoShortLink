CREATE TABLE IF NOT EXISTS schema_migrations (
    version INT UNSIGNED NOT NULL,
    name VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    applied_at TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (version)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE users (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    username VARCHAR(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    username_normalized VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    password_hash VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    status VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT 'active',
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_users_username_normalized (username_normalized),
    CHECK (status IN ('active', 'disabled'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO users (username, username_normalized, password_hash, status)
VALUES ('legacy-system', 'legacy-system', '!', 'disabled');

ALTER TABLE short_links
    ADD COLUMN owner_id BIGINT UNSIGNED NULL AFTER id;

UPDATE short_links
SET owner_id = (SELECT id FROM users WHERE username_normalized = 'legacy-system')
WHERE owner_id IS NULL;

ALTER TABLE short_links
    MODIFY owner_id BIGINT UNSIGNED NOT NULL,
    ADD KEY idx_short_links_owner_id_id (owner_id, id),
    ADD KEY idx_short_links_owner_status_id (owner_id, status, id),
    ADD CONSTRAINT fk_short_links_owner
        FOREIGN KEY (owner_id) REFERENCES users(id);

CREATE TABLE user_sessions (
    token_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    user_id BIGINT UNSIGNED NOT NULL,
    expires_at DATETIME NOT NULL,
    revoked_at DATETIME NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (token_hash),
    KEY idx_user_sessions_user_id (user_id),
    KEY idx_user_sessions_expires_at (expires_at),
    CONSTRAINT fk_user_sessions_user
        FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO schema_migrations (version, name) VALUES
    (1, 'create_short_links'),
    (2, 'make_short_link_code_case_sensitive'),
    (3, 'add_short_link_lifecycle'),
    (4, 'create_access_statistics'),
    (5, 'add_users_sessions_and_ownership');
