CREATE TABLE IF NOT EXISTS role_permissions (
    role_id INTEGER NOT NULL REFERENCES roles(id) ON DELETE CASCADE,
    permission_code TEXT NOT NULL COLLATE NOCASE,
    is_allowed INTEGER NOT NULL DEFAULT 1 CHECK (is_allowed IN (0, 1)),
    PRIMARY KEY (role_id, permission_code)
);

CREATE TABLE IF NOT EXISTS material_images (
    material_id INTEGER PRIMARY KEY REFERENCES materials(id) ON DELETE CASCADE,
    original_file_name TEXT NOT NULL,
    mime_type TEXT NOT NULL DEFAULT '',
    sha256 TEXT NOT NULL,
    image_data BLOB NOT NULL,
    updated_by INTEGER REFERENCES users(id),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);

INSERT OR IGNORE INTO role_permissions(role_id, permission_code)
SELECT id, 'VIEW_INVENTORY' FROM roles;
INSERT OR IGNORE INTO role_permissions(role_id, permission_code)
SELECT id, 'MANAGE_MATERIALS' FROM roles WHERE code IN ('ADMIN', 'WAREHOUSE');
INSERT OR IGNORE INTO role_permissions(role_id, permission_code)
SELECT id, 'MANAGE_WAREHOUSES' FROM roles WHERE code = 'ADMIN';
INSERT OR IGNORE INTO role_permissions(role_id, permission_code)
SELECT id, 'POST_INVENTORY' FROM roles WHERE code IN ('ADMIN', 'WAREHOUSE');
INSERT OR IGNORE INTO role_permissions(role_id, permission_code)
SELECT id, 'POST_PRODUCTION' FROM roles WHERE code IN ('ADMIN', 'WAREHOUSE', 'PRODUCTION');
INSERT OR IGNORE INTO role_permissions(role_id, permission_code)
SELECT id, 'MANAGE_ATTACHMENTS' FROM roles WHERE code IN ('ADMIN', 'WAREHOUSE');
INSERT OR IGNORE INTO role_permissions(role_id, permission_code)
SELECT id, 'MANAGE_USERS' FROM roles WHERE code = 'ADMIN';
INSERT OR IGNORE INTO role_permissions(role_id, permission_code)
SELECT id, 'MANAGE_SYSTEM' FROM roles WHERE code = 'ADMIN';
INSERT OR IGNORE INTO role_permissions(role_id, permission_code)
SELECT id, 'VIEW_AUDIT' FROM roles WHERE code = 'ADMIN';

CREATE INDEX IF NOT EXISTS idx_role_permissions_code ON role_permissions(permission_code, is_allowed);
CREATE INDEX IF NOT EXISTS idx_audit_logs_created_at ON audit_logs(created_at DESC);

INSERT INTO schema_migrations(version) VALUES (3);
