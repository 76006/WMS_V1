INSERT INTO number_rules(document_type, prefix, sequence_width)
VALUES ('SCTL', 'SMTL', 4)
ON CONFLICT(document_type) DO UPDATE SET
    prefix = excluded.prefix;

INSERT INTO schema_migrations(version) VALUES (14);
