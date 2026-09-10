ALTER TABLE sales_outbound_details ADD COLUMN delivery_date TEXT NOT NULL DEFAULT '';

UPDATE sales_outbound_details
SET delivery_date = COALESCE(
    (SELECT d.document_date FROM business_documents d
     WHERE d.id = sales_outbound_details.document_id), '')
WHERE COALESCE(delivery_date, '') = '';

INSERT INTO schema_migrations(version) VALUES (12);
