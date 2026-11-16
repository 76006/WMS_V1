ALTER TABLE number_rules RENAME TO number_rules_before_v21;

CREATE TABLE number_rules (
    document_type TEXT PRIMARY KEY,
    prefix TEXT NOT NULL,
    sequence_date TEXT NOT NULL DEFAULT '',
    current_sequence INTEGER NOT NULL DEFAULT 0 CHECK (current_sequence >= 0),
    sequence_width INTEGER NOT NULL DEFAULT 4 CHECK (sequence_width BETWEEN 2 AND 8)
);

INSERT INTO number_rules(
    document_type,prefix,sequence_date,current_sequence,sequence_width)
SELECT document_type,prefix,sequence_date,current_sequence,sequence_width
FROM number_rules_before_v21;

DROP TABLE number_rules_before_v21;

INSERT INTO schema_migrations(version) VALUES (21);
