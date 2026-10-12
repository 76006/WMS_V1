ALTER TABLE inspection_notice_items
    ADD COLUMN qualified_quantity NUMERIC NOT NULL DEFAULT 0
        CHECK (qualified_quantity >= 0);

ALTER TABLE inspection_notice_items
    ADD COLUMN unqualified_quantity NUMERIC NOT NULL DEFAULT 0
        CHECK (unqualified_quantity >= 0);

ALTER TABLE inspection_notice_items
    ADD COLUMN inbound_quantity NUMERIC NOT NULL DEFAULT 0
        CHECK (inbound_quantity >= 0);

UPDATE inspection_notice_items
SET qualified_quantity = CASE
        WHEN (SELECT inspection_result FROM inspection_notices
              WHERE id=inspection_notice_items.notice_id) = 'QUALIFIED'
        THEN quantity ELSE 0 END,
    unqualified_quantity = CASE
        WHEN (SELECT inspection_result FROM inspection_notices
              WHERE id=inspection_notice_items.notice_id) = 'UNQUALIFIED'
        THEN quantity ELSE 0 END,
    inbound_quantity = CASE
        WHEN (SELECT status FROM inspection_notices
              WHERE id=inspection_notice_items.notice_id) = 'USED'
        THEN quantity ELSE 0 END;

UPDATE inspection_notices
SET status = 'PENDING'
WHERE status = 'UNQUALIFIED' AND linked_document_id IS NULL;

DROP INDEX IF EXISTS idx_inbound_inspection_notice;
DROP INDEX IF EXISTS idx_inbound_inspection_no;

CREATE INDEX IF NOT EXISTS idx_inbound_inspection_notice
    ON inbound_inspection_details(inspection_notice_id);

CREATE INDEX IF NOT EXISTS idx_inbound_inspection_no
    ON inbound_inspection_details(inspection_no)
    WHERE inspection_no <> '';

INSERT INTO schema_migrations(version) VALUES (18);
