CREATE TABLE production_task_boms (
    production_run_id INTEGER PRIMARY KEY REFERENCES production_runs(id) ON DELETE CASCADE,
    payload TEXT NOT NULL
);

CREATE TABLE production_document_stages (
    document_id INTEGER PRIMARY KEY REFERENCES business_documents(id) ON DELETE CASCADE,
    production_run_id INTEGER NOT NULL REFERENCES production_runs(id) ON DELETE CASCADE,
    stage_key TEXT NOT NULL
);
CREATE INDEX idx_production_document_stage ON production_document_stages(production_run_id, stage_key);

INSERT INTO schema_migrations(version) VALUES (23);
