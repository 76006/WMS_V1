CREATE TABLE IF NOT EXISTS issue_line_options (
    document_item_id INTEGER PRIMARY KEY
        REFERENCES business_document_items(id) ON DELETE CASCADE,
    external_dispatch INTEGER NOT NULL DEFAULT 0
        CHECK (external_dispatch IN (0, 1)),
    secondary_dispatch INTEGER NOT NULL DEFAULT 0
        CHECK (secondary_dispatch IN (0, 1)),
    loss_marked INTEGER NOT NULL DEFAULT 0
        CHECK (loss_marked IN (0, 1)),
    return_to_stock INTEGER NOT NULL DEFAULT 0
        CHECK (return_to_stock IN (0, 1))
);

INSERT OR IGNORE INTO number_rules(document_type, prefix, sequence_width) VALUES
    ('SHCK', 'SHCK', 4),
    ('YFCK', 'YFCK', 4);

INSERT INTO schema_migrations(version) VALUES (16);
