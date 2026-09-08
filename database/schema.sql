PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);

CREATE TABLE IF NOT EXISTS roles (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    code TEXT NOT NULL COLLATE NOCASE UNIQUE,
    name TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    role_id INTEGER NOT NULL REFERENCES roles(id),
    username TEXT NOT NULL COLLATE NOCASE UNIQUE,
    display_name TEXT NOT NULL,
    password_hash TEXT NOT NULL,
    is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)),
    last_login_at TEXT,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);

CREATE TABLE IF NOT EXISTS material_categories (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    code TEXT NOT NULL COLLATE NOCASE UNIQUE,
    name TEXT NOT NULL UNIQUE,
    sort_order INTEGER NOT NULL DEFAULT 0,
    is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1))
);

CREATE TABLE IF NOT EXISTS material_projects (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    code TEXT NOT NULL COLLATE NOCASE UNIQUE,
    name TEXT NOT NULL DEFAULT '',
    is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);

CREATE TABLE IF NOT EXISTS warehouses (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    code TEXT NOT NULL COLLATE NOCASE UNIQUE,
    name TEXT NOT NULL UNIQUE,
    is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)),
    notes TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);

CREATE TABLE IF NOT EXISTS locations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    warehouse_id INTEGER NOT NULL REFERENCES warehouses(id),
    code TEXT NOT NULL COLLATE NOCASE,
    name TEXT NOT NULL DEFAULT '',
    is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)),
    notes TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    UNIQUE (warehouse_id, code)
);

CREATE TABLE IF NOT EXISTS materials (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    code TEXT NOT NULL COLLATE NOCASE UNIQUE,
    name TEXT NOT NULL,
    specification TEXT NOT NULL DEFAULT '',
    category_id INTEGER REFERENCES material_categories(id),
    brand TEXT NOT NULL DEFAULT '',
    unit TEXT NOT NULL,
    minimum_stock NUMERIC NOT NULL DEFAULT 0 CHECK (minimum_stock >= 0),
    default_warehouse_id INTEGER REFERENCES warehouses(id),
    default_location_id INTEGER REFERENCES locations(id),
    require_batch INTEGER NOT NULL DEFAULT 0 CHECK (require_batch IN (0, 1)),
    require_serial INTEGER NOT NULL DEFAULT 0 CHECK (require_serial IN (0, 1)),
    notes TEXT NOT NULL DEFAULT '',
    is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);

CREATE TABLE IF NOT EXISTS number_rules (
    document_type TEXT PRIMARY KEY,
    prefix TEXT NOT NULL,
    sequence_date TEXT NOT NULL DEFAULT '',
    current_sequence INTEGER NOT NULL DEFAULT 0 CHECK (current_sequence >= 0),
    sequence_width INTEGER NOT NULL DEFAULT 4 CHECK (sequence_width BETWEEN 3 AND 8)
);

CREATE TABLE IF NOT EXISTS business_documents (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    document_no TEXT NOT NULL COLLATE NOCASE UNIQUE,
    document_type TEXT NOT NULL,
    stock_direction TEXT NOT NULL CHECK (stock_direction IN ('IN', 'OUT', 'TRANSFER', 'ADJUST')),
    document_date TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'DRAFT' CHECK (status IN ('DRAFT', 'POSTED', 'PARTIALLY_REVERSED', 'REVERSED')),
    source_document_id INTEGER REFERENCES business_documents(id),
    handler_name TEXT NOT NULL DEFAULT '',
    purpose TEXT NOT NULL DEFAULT '',
    notes TEXT NOT NULL DEFAULT '',
    created_by INTEGER NOT NULL REFERENCES users(id),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    posted_at TEXT,
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);

CREATE TABLE IF NOT EXISTS business_document_items (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    document_id INTEGER NOT NULL REFERENCES business_documents(id) ON DELETE RESTRICT,
    line_number INTEGER NOT NULL,
    material_id INTEGER NOT NULL REFERENCES materials(id),
    quantity NUMERIC NOT NULL CHECK (quantity > 0),
    reversed_quantity NUMERIC NOT NULL DEFAULT 0 CHECK (reversed_quantity >= 0),
    returned_quantity NUMERIC NOT NULL DEFAULT 0 CHECK (returned_quantity >= 0),
    batch_no TEXT NOT NULL DEFAULT '',
    warehouse_id INTEGER REFERENCES warehouses(id),
    location_id INTEGER REFERENCES locations(id),
    target_warehouse_id INTEGER REFERENCES warehouses(id),
    target_location_id INTEGER REFERENCES locations(id),
    notes TEXT NOT NULL DEFAULT '',
    UNIQUE (document_id, line_number),
    CHECK (reversed_quantity <= quantity),
    CHECK (returned_quantity <= quantity)
);

CREATE TABLE IF NOT EXISTS stock_balances (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    material_id INTEGER NOT NULL REFERENCES materials(id),
    warehouse_id INTEGER NOT NULL REFERENCES warehouses(id),
    location_id INTEGER NOT NULL REFERENCES locations(id),
    batch_no TEXT NOT NULL DEFAULT '',
    quantity NUMERIC NOT NULL DEFAULT 0 CHECK (quantity >= 0),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    UNIQUE (material_id, warehouse_id, location_id, batch_no)
);

CREATE TABLE IF NOT EXISTS inventory_ledger (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    occurred_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    business_type TEXT NOT NULL,
    document_id INTEGER NOT NULL REFERENCES business_documents(id),
    document_item_id INTEGER NOT NULL REFERENCES business_document_items(id),
    material_id INTEGER NOT NULL REFERENCES materials(id),
    batch_no TEXT NOT NULL DEFAULT '',
    serial_no TEXT NOT NULL DEFAULT '',
    quantity_in NUMERIC NOT NULL DEFAULT 0 CHECK (quantity_in >= 0),
    quantity_out NUMERIC NOT NULL DEFAULT 0 CHECK (quantity_out >= 0),
    quantity_before NUMERIC NOT NULL CHECK (quantity_before >= 0),
    quantity_after NUMERIC NOT NULL CHECK (quantity_after >= 0),
    warehouse_id INTEGER NOT NULL REFERENCES warehouses(id),
    location_id INTEGER NOT NULL REFERENCES locations(id),
    operator_id INTEGER NOT NULL REFERENCES users(id),
    reversal_of_ledger_id INTEGER REFERENCES inventory_ledger(id),
    notes TEXT NOT NULL DEFAULT '',
    CHECK ((quantity_in > 0 AND quantity_out = 0) OR (quantity_out > 0 AND quantity_in = 0))
);

CREATE TABLE IF NOT EXISTS batches (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    material_id INTEGER NOT NULL REFERENCES materials(id),
    batch_no TEXT NOT NULL,
    supplier TEXT NOT NULL DEFAULT '',
    first_in_at TEXT,
    notes TEXT NOT NULL DEFAULT '',
    UNIQUE (material_id, batch_no)
);

CREATE TABLE IF NOT EXISTS serial_numbers (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    material_id INTEGER NOT NULL REFERENCES materials(id),
    serial_no TEXT NOT NULL COLLATE NOCASE UNIQUE,
    batch_no TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL CHECK (status IN ('IN_STOCK', 'OUTBOUND', 'CONSUMED', 'SCRAPPED', 'VOIDED')),
    warehouse_id INTEGER REFERENCES warehouses(id),
    location_id INTEGER REFERENCES locations(id),
    production_batch TEXT NOT NULL DEFAULT '',
    inbound_at TEXT,
    outbound_at TEXT,
    last_document_id INTEGER REFERENCES business_documents(id),
    notes TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS inventory_ledger_serials (
    ledger_id INTEGER NOT NULL REFERENCES inventory_ledger(id) ON DELETE RESTRICT,
    serial_id INTEGER NOT NULL REFERENCES serial_numbers(id) ON DELETE RESTRICT,
    PRIMARY KEY (ledger_id, serial_id)
);

CREATE TABLE IF NOT EXISTS inventory_counts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    document_id INTEGER NOT NULL UNIQUE REFERENCES business_documents(id),
    status TEXT NOT NULL DEFAULT 'DRAFT' CHECK (status IN ('DRAFT', 'CONFIRMED', 'CANCELLED')),
    confirmed_at TEXT
);

CREATE TABLE IF NOT EXISTS inventory_count_items (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    inventory_count_id INTEGER NOT NULL REFERENCES inventory_counts(id),
    material_id INTEGER NOT NULL REFERENCES materials(id),
    warehouse_id INTEGER NOT NULL REFERENCES warehouses(id),
    location_id INTEGER NOT NULL REFERENCES locations(id),
    batch_no TEXT NOT NULL DEFAULT '',
    system_quantity NUMERIC NOT NULL,
    actual_quantity NUMERIC,
    difference_quantity NUMERIC,
    difference_reason TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS attachments (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    business_type TEXT NOT NULL,
    business_id INTEGER NOT NULL,
    original_file_name TEXT NOT NULL,
    mime_type TEXT NOT NULL DEFAULT '',
    file_size INTEGER NOT NULL CHECK (file_size >= 0),
    sha256 TEXT NOT NULL DEFAULT '',
    file_data BLOB NOT NULL,
    uploaded_by INTEGER NOT NULL REFERENCES users(id),
    uploaded_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    is_deleted INTEGER NOT NULL DEFAULT 0 CHECK (is_deleted IN (0, 1)),
    deleted_by INTEGER REFERENCES users(id),
    deleted_at TEXT
);

CREATE TABLE IF NOT EXISTS audit_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER REFERENCES users(id),
    action TEXT NOT NULL,
    entity_type TEXT NOT NULL,
    entity_id INTEGER,
    detail TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);

CREATE INDEX IF NOT EXISTS idx_materials_name ON materials(name);
CREATE INDEX IF NOT EXISTS idx_documents_date_type ON business_documents(document_date, document_type);
CREATE INDEX IF NOT EXISTS idx_ledger_material_time ON inventory_ledger(material_id, occurred_at DESC);
CREATE INDEX IF NOT EXISTS idx_ledger_document ON inventory_ledger(document_id);
CREATE INDEX IF NOT EXISTS idx_stock_location ON stock_balances(warehouse_id, location_id);
CREATE INDEX IF NOT EXISTS idx_serial_material_status ON serial_numbers(material_id, status);
CREATE INDEX IF NOT EXISTS idx_attachments_business ON attachments(business_type, business_id, is_deleted);

INSERT OR IGNORE INTO roles(code, name, description) VALUES
    ('ADMIN', '管理员', '全部功能'),
    ('WAREHOUSE', '仓库人员', '入库、出库、调拨和盘点'),
    ('PRODUCTION', '生产人员', '库存与生产业务查询'),
    ('QUERY', '查询人员', '只读查询');

INSERT OR IGNORE INTO material_categories(code, name, sort_order) VALUES
    ('RAW', '原材料', 10),
    ('ELECTRONIC', '电子元器件', 20),
    ('STRUCTURE', '结构件', 30),
    ('SEMI', '半成品', 40),
    ('FINISHED', '成品', 50),
    ('PACKAGING', '包装材料', 60),
    ('CONSUMABLE', '辅料耗材', 70),
    ('DEFECTIVE', '不良品', 80),
    ('SPARE', '备件', 90);

INSERT OR IGNORE INTO material_projects(code, name) VALUES ('SM01', 'SM01项目');

INSERT OR IGNORE INTO number_rules(document_type, prefix, sequence_width) VALUES
    ('CGRK', 'CGRK', 4),
    ('SCWG', 'SCWG', 4),
    ('TLRK', 'TLRK', 4),
    ('QTRK', 'QTRK', 4),
    ('QC', 'QC', 4),
    ('SCLL', 'SCLL', 4),
    ('SCTL', 'SCTL', 4),
    ('CPRK', 'CPRK', 4),
    ('XSCK', 'XSCK', 4),
    ('WXLY', 'WXLY', 4),
    ('YPLY', 'YPLY', 4),
    ('QTCK', 'QTCK', 4),
    ('DB', 'DB', 4),
    ('PD', 'PD', 4),
    ('CX', 'CX', 4);

INSERT OR IGNORE INTO schema_migrations(version) VALUES (1);
