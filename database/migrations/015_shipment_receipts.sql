ALTER TABLE sales_outbound_details
    ADD COLUMN receipt_status TEXT NOT NULL DEFAULT 'PENDING'
        CHECK (receipt_status IN ('PENDING', 'SIGNED'));

ALTER TABLE sales_outbound_details
    ADD COLUMN receipt_date TEXT;

ALTER TABLE sales_outbound_details
    ADD COLUMN receipt_attachment_id INTEGER REFERENCES attachments(id);

ALTER TABLE sales_outbound_details
    ADD COLUMN receipt_confirmed_by INTEGER REFERENCES users(id);

ALTER TABLE sales_outbound_details
    ADD COLUMN receipt_confirmed_at TEXT;

CREATE INDEX IF NOT EXISTS idx_sales_outbound_receipt_status
    ON sales_outbound_details(receipt_status, receipt_date DESC);

INSERT INTO schema_migrations(version) VALUES (15);
