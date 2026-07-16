ALTER TABLE short_links
    ADD COLUMN status VARCHAR(16) NOT NULL DEFAULT 'active' AFTER original_url,
    ADD COLUMN expires_at DATETIME NULL AFTER status,
    ADD CHECK (status IN ('active', 'disabled')),
    ADD KEY idx_short_links_status_id (status, id);
