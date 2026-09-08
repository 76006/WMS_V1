CREATE TABLE IF NOT EXISTS material_projects (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    code TEXT NOT NULL COLLATE NOCASE UNIQUE,
    name TEXT NOT NULL DEFAULT '',
    is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);

INSERT OR IGNORE INTO material_projects(code, name) VALUES ('SM01', 'SM01项目');

CREATE INDEX IF NOT EXISTS idx_material_projects_active
    ON material_projects(is_active, code);

INSERT INTO schema_migrations(version) VALUES (4);
