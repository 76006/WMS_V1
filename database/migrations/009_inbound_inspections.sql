CREATE TABLE IF NOT EXISTS inbound_inspection_details (
    document_id INTEGER PRIMARY KEY REFERENCES business_documents(id) ON DELETE RESTRICT,
    requires_inspection INTEGER NOT NULL DEFAULT 0 CHECK (requires_inspection IN (0, 1)),
    inspection_no TEXT NOT NULL DEFAULT '',
    inspection_date TEXT,
    inspector_name TEXT NOT NULL DEFAULT '',
    inspection_result TEXT NOT NULL DEFAULT 'NOT_REQUIRED'
        CHECK (inspection_result IN ('NOT_REQUIRED', 'PENDING', 'QUALIFIED', 'UNQUALIFIED')),
    conclusion TEXT NOT NULL DEFAULT '',
    inspection_attachment_id INTEGER REFERENCES attachments(id)
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_inbound_inspection_no
    ON inbound_inspection_details(inspection_no)
    WHERE inspection_no <> '';

CREATE INDEX IF NOT EXISTS idx_inbound_inspection_result
    ON inbound_inspection_details(requires_inspection, inspection_result);

INSERT INTO schema_migrations(version) VALUES (9);
