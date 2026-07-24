DROP TABLE user_sessions;

ALTER TABLE short_links
    DROP FOREIGN KEY fk_short_links_owner,
    DROP INDEX idx_short_links_owner_status_id,
    DROP INDEX idx_short_links_owner_id_id,
    DROP COLUMN owner_id;

DROP TABLE users;

DELETE FROM schema_migrations WHERE version = 5;
