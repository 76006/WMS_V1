ALTER TABLE business_documents
    ADD COLUMN supplier TEXT NOT NULL DEFAULT '';

ALTER TABLE business_document_items
    ADD COLUMN ordered_quantity NUMERIC NOT NULL DEFAULT 0 CHECK (ordered_quantity >= 0);

ALTER TABLE business_document_items
    ADD COLUMN gift_quantity NUMERIC NOT NULL DEFAULT 0 CHECK (gift_quantity >= 0);

ALTER TABLE business_document_items
    ADD COLUMN reversed_gift_quantity NUMERIC NOT NULL DEFAULT 0 CHECK (reversed_gift_quantity >= 0);

CREATE INDEX IF NOT EXISTS idx_documents_reporting
    ON business_documents(document_date, document_type, stock_direction);

CREATE INDEX IF NOT EXISTS idx_items_material_document
    ON business_document_items(material_id, document_id);

INSERT INTO schema_migrations(version) VALUES (5);
