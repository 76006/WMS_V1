-- 保留现有生产批次 ID 和单据外键，在同一事务内重建批次唯一约束。
PRAGMA defer_foreign_keys = ON;
CREATE TEMP TABLE previous_runs AS SELECT * FROM production_runs;
CREATE TEMP TABLE previous_run_sequence AS
    SELECT seq FROM sqlite_sequence WHERE name='production_runs';
DROP TABLE production_runs;
CREATE TABLE production_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    batch_no TEXT NOT NULL COLLATE NOCASE,
    product_material_id INTEGER NOT NULL REFERENCES materials(id),
    product_name TEXT NOT NULL,
    product_model TEXT NOT NULL DEFAULT '',
    planned_quantity NUMERIC NOT NULL CHECK (planned_quantity > 0),
    status TEXT NOT NULL DEFAULT 'OPEN' CHECK (status IN ('OPEN', 'COMPLETED')),
    created_by INTEGER NOT NULL REFERENCES users(id),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    UNIQUE (batch_no, product_material_id)
);
INSERT INTO production_runs SELECT * FROM previous_runs;
UPDATE sqlite_sequence SET seq = (SELECT seq FROM previous_run_sequence)
    WHERE name='production_runs' AND seq < (SELECT seq FROM previous_run_sequence);
DROP TABLE previous_run_sequence;
DROP TABLE previous_runs;

ALTER TABLE business_documents ADD COLUMN issue_unit_count NUMERIC CHECK (issue_unit_count > 0);
INSERT INTO schema_migrations(version) VALUES (22);
