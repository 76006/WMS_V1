ALTER TABLE materials
    ADD COLUMN unit_usage NUMERIC NOT NULL DEFAULT 0 CHECK (unit_usage >= 0);

ALTER TABLE materials
    ADD COLUMN processing_method TEXT NOT NULL DEFAULT '';

INSERT INTO schema_migrations(version) VALUES (6);
