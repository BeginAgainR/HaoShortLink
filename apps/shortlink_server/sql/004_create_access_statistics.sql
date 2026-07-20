CREATE TABLE IF NOT EXISTS processed_access_events (
    event_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    source_topic VARCHAR(249) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    source_partition INT NOT NULL,
    source_offset BIGINT NOT NULL,
    occurred_at_ms BIGINT UNSIGNED NOT NULL,
    disposition VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    processed_at TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (event_id),
    KEY idx_processed_access_events_source
        (source_topic, source_partition, source_offset),
    KEY idx_processed_access_events_processed_at (processed_at),
    CHECK (source_partition >= 0),
    CHECK (source_offset >= 0),
    CHECK (occurred_at_ms > 0),
    CHECK (disposition IN ('received', 'aggregated', 'not_found_ignored'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS short_link_access_totals (
    short_link_id BIGINT UNSIGNED NOT NULL,
    result VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    access_count BIGINT UNSIGNED NOT NULL DEFAULT 0,
    first_occurred_at_ms BIGINT UNSIGNED NOT NULL,
    last_occurred_at_ms BIGINT UNSIGNED NOT NULL,
    updated_at TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
        ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (short_link_id, result),
    CONSTRAINT fk_short_link_access_totals_short_link
        FOREIGN KEY (short_link_id) REFERENCES short_links(id),
    CHECK (result IN ('success', 'disabled', 'expired', 'error')),
    CHECK (access_count > 0),
    CHECK (first_occurred_at_ms > 0),
    CHECK (last_occurred_at_ms >= first_occurred_at_ms)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS short_link_access_hourly (
    short_link_id BIGINT UNSIGNED NOT NULL,
    bucket_start_epoch BIGINT UNSIGNED NOT NULL,
    result VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    access_count BIGINT UNSIGNED NOT NULL DEFAULT 0,
    updated_at TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
        ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (short_link_id, bucket_start_epoch, result),
    CONSTRAINT fk_short_link_access_hourly_short_link
        FOREIGN KEY (short_link_id) REFERENCES short_links(id),
    CHECK (MOD(bucket_start_epoch, 3600) = 0),
    CHECK (result IN ('success', 'disabled', 'expired', 'error')),
    CHECK (access_count > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
