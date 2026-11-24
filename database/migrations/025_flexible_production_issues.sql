-- Foreign keys are checked by the caller before commit. Preserve the high watermark
-- even when the last production run was deleted.
CREATE TEMP TABLE previous_run_sequence AS
    SELECT seq FROM sqlite_sequence WHERE name='production_runs';
CREATE TABLE production_runs_v25 (
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
INSERT INTO production_runs_v25 SELECT * FROM production_runs;
DROP TABLE production_runs;
ALTER TABLE production_runs_v25 RENAME TO production_runs;
UPDATE sqlite_sequence SET seq=(SELECT seq FROM previous_run_sequence)
    WHERE name='production_runs' AND seq < (SELECT seq FROM previous_run_sequence);
DROP TABLE previous_run_sequence;
