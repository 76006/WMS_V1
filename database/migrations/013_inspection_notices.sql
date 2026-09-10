CREATE TABLE IF NOT EXISTS inspection_notices (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    inspection_no TEXT NOT NULL COLLATE NOCASE UNIQUE,
    status TEXT NOT NULL DEFAULT 'PENDING'
        CHECK (status IN ('PENDING', 'QUALIFIED', 'UNQUALIFIED', 'USED', 'CANCELLED')),
    notification_date TEXT NOT NULL,
    arrival_date TEXT NOT NULL,
    entrusted_by TEXT NOT NULL,
    notification_department TEXT NOT NULL,
    urgency TEXT NOT NULL DEFAULT 'NORMAL'
        CHECK (urgency IN ('EXPEDITED', 'URGENT', 'NORMAL')),
    purchase_order_no TEXT NOT NULL DEFAULT '',
    supplier TEXT NOT NULL DEFAULT '',
    inspector_name TEXT NOT NULL DEFAULT '',
    inspection_date TEXT,
    inspection_result TEXT NOT NULL DEFAULT 'PENDING'
        CHECK (inspection_result IN ('PENDING', 'QUALIFIED', 'UNQUALIFIED')),
    conclusion TEXT NOT NULL DEFAULT '',
    inspection_attachment_id INTEGER REFERENCES attachments(id),
    template_file_attachment_id INTEGER REFERENCES attachments(id),
    template_payload TEXT NOT NULL DEFAULT '',
    archive_path TEXT NOT NULL DEFAULT '',
    linked_document_id INTEGER UNIQUE REFERENCES business_documents(id) ON DELETE RESTRICT,
    created_by INTEGER NOT NULL REFERENCES users(id),
    updated_by INTEGER REFERENCES users(id),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    completed_at TEXT
);

CREATE TABLE IF NOT EXISTS inspection_notice_items (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    notice_id INTEGER NOT NULL REFERENCES inspection_notices(id) ON DELETE CASCADE,
    line_number INTEGER NOT NULL CHECK (line_number > 0),
    material_id INTEGER NOT NULL REFERENCES materials(id) ON DELETE RESTRICT,
    quantity NUMERIC NOT NULL CHECK (quantity > 0),
    purchase_order_no TEXT NOT NULL DEFAULT '',
    batch_no TEXT NOT NULL DEFAULT '',
    supplier TEXT NOT NULL DEFAULT '',
    UNIQUE (notice_id, line_number)
);

ALTER TABLE inbound_inspection_details
    ADD COLUMN inspection_notice_id INTEGER REFERENCES inspection_notices(id) ON DELETE RESTRICT;

CREATE UNIQUE INDEX IF NOT EXISTS idx_inbound_inspection_notice
    ON inbound_inspection_details(inspection_notice_id)
    WHERE inspection_notice_id IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_inspection_notices_status_date
    ON inspection_notices(status, notification_date DESC, id DESC);

CREATE INDEX IF NOT EXISTS idx_inspection_notice_items_notice
    ON inspection_notice_items(notice_id, line_number);

INSERT INTO schema_migrations(version) VALUES (13);
