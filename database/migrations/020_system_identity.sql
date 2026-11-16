CREATE TABLE IF NOT EXISTS system_identity (
    singleton_id INTEGER PRIMARY KEY CHECK (singleton_id = 1),
    profile_id TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);

INSERT INTO schema_migrations(version) VALUES (20);
