CREATE TABLE IF NOT EXISTS material_bom_items (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    product_material_id INTEGER NOT NULL REFERENCES materials(id) ON DELETE CASCADE,
    parent_item_id INTEGER REFERENCES material_bom_items(id) ON DELETE CASCADE,
    component_material_id INTEGER NOT NULL REFERENCES materials(id) ON DELETE RESTRICT,
    quantity NUMERIC NOT NULL DEFAULT 1 CHECK (quantity > 0),
    sort_order INTEGER NOT NULL DEFAULT 0,
    source_sheet TEXT NOT NULL DEFAULT '',
    source_row INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime'))
);

CREATE INDEX IF NOT EXISTS idx_material_bom_product_parent
    ON material_bom_items(product_material_id, parent_item_id, sort_order, id);

CREATE INDEX IF NOT EXISTS idx_material_bom_component
    ON material_bom_items(component_material_id);

INSERT INTO schema_migrations(version) VALUES (10);
