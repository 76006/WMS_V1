CREATE TABLE IF NOT EXISTS document_forms (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    document_id INTEGER NOT NULL REFERENCES business_documents(id) ON DELETE RESTRICT,
    form_kind TEXT NOT NULL,
    payload TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'PENDING'
        CHECK (status IN ('PENDING', 'FAILED', 'COMPLETED')),
    last_error TEXT NOT NULL DEFAULT '',
    attachment_id INTEGER REFERENCES attachments(id),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    UNIQUE (document_id, form_kind)
);

CREATE INDEX IF NOT EXISTS idx_document_forms_document_status
    ON document_forms(document_id, status);

INSERT INTO schema_migrations(version) VALUES (11);
