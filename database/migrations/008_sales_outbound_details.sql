CREATE TABLE IF NOT EXISTS sales_outbound_details (
    document_id INTEGER PRIMARY KEY REFERENCES business_documents(id) ON DELETE RESTRICT,
    customer_company TEXT NOT NULL,
    destination TEXT NOT NULL,
    contact_name TEXT NOT NULL DEFAULT '',
    contact_phone TEXT NOT NULL DEFAULT '',
    sales_order_no TEXT NOT NULL DEFAULT '',
    logistics_company TEXT NOT NULL DEFAULT '',
    tracking_no TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_sales_outbound_customer
    ON sales_outbound_details(customer_company);

CREATE INDEX IF NOT EXISTS idx_sales_outbound_order
    ON sales_outbound_details(sales_order_no);

INSERT INTO schema_migrations(version) VALUES (8);
