CREATE TABLE IF NOT EXISTS production_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    batch_no TEXT NOT NULL COLLATE NOCASE UNIQUE,
    product_material_id INTEGER NOT NULL REFERENCES materials(id),
    product_name TEXT NOT NULL,
    product_model TEXT NOT NULL DEFAULT '',
    planned_quantity NUMERIC NOT NULL CHECK (planned_quantity > 0),
    status TEXT NOT NULL DEFAULT 'OPEN' CHECK (status IN ('OPEN', 'COMPLETED')),
    created_by INTEGER NOT NULL REFERENCES users(id),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);

ALTER TABLE business_documents
    ADD COLUMN production_run_id INTEGER REFERENCES production_runs(id);

ALTER TABLE business_documents
    ADD COLUMN submission_token TEXT COLLATE NOCASE;

ALTER TABLE business_document_items
    ADD COLUMN source_item_id INTEGER REFERENCES business_document_items(id);

CREATE UNIQUE INDEX IF NOT EXISTS idx_documents_submission_token
    ON business_documents(submission_token)
    WHERE submission_token IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_documents_production_run
    ON business_documents(production_run_id, document_type, document_date);

CREATE INDEX IF NOT EXISTS idx_items_source_item
    ON business_document_items(source_item_id);

INSERT INTO schema_migrations(version) VALUES (2);
