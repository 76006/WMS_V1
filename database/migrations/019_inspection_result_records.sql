CREATE TABLE IF NOT EXISTS inspection_results (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    notice_id INTEGER NOT NULL REFERENCES inspection_notices(id) ON DELETE CASCADE,
    sequence_no INTEGER NOT NULL CHECK (sequence_no > 0),
    inspection_date TEXT NOT NULL,
    inspector_name TEXT NOT NULL,
    conclusion TEXT NOT NULL DEFAULT '',
    attachment_id INTEGER REFERENCES attachments(id),
    created_by INTEGER NOT NULL REFERENCES users(id),
    updated_by INTEGER REFERENCES users(id),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', 'localtime')),
    UNIQUE (notice_id, sequence_no)
);

CREATE TABLE IF NOT EXISTS inspection_result_items (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    result_id INTEGER NOT NULL REFERENCES inspection_results(id) ON DELETE CASCADE,
    notice_item_id INTEGER NOT NULL,
    line_number INTEGER NOT NULL CHECK (line_number > 0),
    material_id INTEGER NOT NULL REFERENCES materials(id) ON DELETE RESTRICT,
    material_code TEXT NOT NULL DEFAULT '',
    material_name TEXT NOT NULL DEFAULT '',
    specification TEXT NOT NULL DEFAULT '',
    batch_no TEXT NOT NULL DEFAULT '',
    inspected_quantity NUMERIC NOT NULL CHECK (inspected_quantity >= 0),
    qualified_quantity NUMERIC NOT NULL CHECK (qualified_quantity >= 0),
    unqualified_quantity NUMERIC NOT NULL CHECK (unqualified_quantity >= 0),
    UNIQUE (result_id, line_number)
);

CREATE INDEX IF NOT EXISTS idx_inspection_results_notice
    ON inspection_results(notice_id, sequence_no);

CREATE INDEX IF NOT EXISTS idx_inspection_result_items_result
    ON inspection_result_items(result_id, line_number);

INSERT INTO inspection_results(
    notice_id,sequence_no,inspection_date,inspector_name,conclusion,attachment_id,
    created_by,updated_by,created_at,updated_at)
SELECT n.id,1,COALESCE(n.inspection_date,n.notification_date),
       CASE WHEN TRIM(n.inspector_name)='' THEN COALESCE(u.display_name,'') ELSE n.inspector_name END,
       n.conclusion,n.inspection_attachment_id,n.created_by,COALESCE(n.updated_by,n.created_by),
       COALESCE(n.completed_at,n.updated_at,n.created_at),COALESCE(n.updated_at,n.created_at)
FROM inspection_notices n
LEFT JOIN users u ON u.id=n.created_by
WHERE (n.inspection_date IS NOT NULL OR EXISTS(
          SELECT 1 FROM inspection_notice_items i
          WHERE i.notice_id=n.id
            AND (i.qualified_quantity>0.0000001 OR i.unqualified_quantity>0.0000001)))
  AND NOT EXISTS(SELECT 1 FROM inspection_results r WHERE r.notice_id=n.id);

INSERT INTO inspection_result_items(
    result_id,notice_item_id,line_number,material_id,material_code,material_name,
    specification,batch_no,inspected_quantity,qualified_quantity,unqualified_quantity)
SELECT r.id,i.id,i.line_number,i.material_id,m.code,m.name,m.specification,i.batch_no,
       i.qualified_quantity+i.unqualified_quantity,
       i.qualified_quantity,i.unqualified_quantity
FROM inspection_results r
JOIN inspection_notice_items i ON i.notice_id=r.notice_id
JOIN materials m ON m.id=i.material_id
WHERE r.sequence_no=1
  AND (i.qualified_quantity>0.0000001 OR i.unqualified_quantity>0.0000001)
  AND NOT EXISTS(SELECT 1 FROM inspection_result_items ri WHERE ri.result_id=r.id);

INSERT INTO schema_migrations(version) VALUES (19);
