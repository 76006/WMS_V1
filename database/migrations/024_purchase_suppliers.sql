CREATE TABLE purchase_item_suppliers (
    document_item_id INTEGER PRIMARY KEY REFERENCES business_document_items(id) ON DELETE CASCADE,
    supplier TEXT NOT NULL DEFAULT ''
);
CREATE TABLE material_supplier_defaults (
    material_id INTEGER PRIMARY KEY REFERENCES materials(id) ON DELETE CASCADE,
    original_brand TEXT NOT NULL DEFAULT ''
);

INSERT INTO purchase_item_suppliers(document_item_id,supplier)
SELECT i.id,COALESCE(NULLIF((SELECT n.supplier FROM inspection_notice_items n
    JOIN inbound_inspection_details q ON q.inspection_notice_id=n.notice_id
    WHERE q.document_id=d.id AND n.material_id=i.material_id AND n.batch_no=i.batch_no
    AND n.supplier<>'' ORDER BY n.id DESC LIMIT 1),''),
    CASE WHEN (SELECT COUNT(*) FROM business_document_items old_i
        JOIN business_documents old_d ON old_d.id=old_i.document_id
        WHERE old_i.material_id=i.material_id AND old_i.batch_no=i.batch_no AND old_d.document_type='CGRK')=1
        THEN NULLIF(b.supplier,'') ELSE NULL END,NULLIF(d.supplier,''),'')
FROM business_document_items i JOIN business_documents d ON d.id=i.document_id
LEFT JOIN batches b ON b.material_id=i.material_id AND b.batch_no=i.batch_no
WHERE d.document_type='CGRK';

INSERT INTO material_supplier_defaults(material_id,original_brand)
SELECT m.id,m.brand FROM materials m WHERE EXISTS(
    SELECT 1 FROM purchase_item_suppliers s JOIN business_document_items i ON i.id=s.document_item_id
    WHERE i.material_id=m.id);

UPDATE materials SET brand=COALESCE((SELECT s.supplier FROM purchase_item_suppliers s
    JOIN business_document_items i ON i.id=s.document_item_id
    JOIN business_documents d ON d.id=i.document_id
    WHERE i.material_id=materials.id AND d.document_type='CGRK'
    AND d.status IN ('POSTED','PARTIALLY_REVERSED') AND i.quantity-i.reversed_quantity>0.0000001
    AND s.supplier<>'' ORDER BY d.document_date DESC,d.id DESC,i.id DESC LIMIT 1),brand);

UPDATE batches SET supplier=COALESCE((SELECT s.supplier FROM purchase_item_suppliers s
    JOIN business_document_items i ON i.id=s.document_item_id JOIN business_documents d ON d.id=i.document_id
    WHERE i.material_id=batches.material_id AND i.batch_no=batches.batch_no AND d.document_type='CGRK'
    AND d.status IN ('POSTED','PARTIALLY_REVERSED') AND i.quantity-i.reversed_quantity>0.0000001
    AND s.supplier<>'' ORDER BY d.document_date DESC,d.id DESC,i.id DESC LIMIT 1),'')
WHERE EXISTS(SELECT 1 FROM purchase_item_suppliers s JOIN business_document_items i ON i.id=s.document_item_id
    WHERE i.material_id=batches.material_id AND i.batch_no=batches.batch_no);

INSERT INTO schema_migrations(version) VALUES (24);
