CREATE TABLE IF NOT EXISTS issue_line_usage (
    document_item_id INTEGER PRIMARY KEY
        REFERENCES business_document_items(id) ON DELETE CASCADE,
    unit_usage NUMERIC NOT NULL DEFAULT -1
        CHECK (unit_usage >= -1)
);

INSERT INTO schema_migrations(version) VALUES (17);
