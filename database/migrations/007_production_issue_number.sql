INSERT INTO number_rules(document_type, prefix, sequence_width)
VALUES ('SCLL', 'SMLL', 3)
ON CONFLICT(document_type) DO UPDATE SET
    prefix = excluded.prefix,
    sequence_width = excluded.sequence_width;

INSERT INTO schema_migrations(version) VALUES (7);
