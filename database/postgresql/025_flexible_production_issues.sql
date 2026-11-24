ALTER TABLE production_runs DROP CONSTRAINT production_runs_batch_no_key,
    ADD UNIQUE (batch_no, product_material_id);
ALTER TABLE business_documents ADD COLUMN issue_unit_count NUMERIC CHECK (issue_unit_count > 0);
INSERT INTO schema_migrations(version) VALUES (25);
