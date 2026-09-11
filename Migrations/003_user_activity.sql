BEGIN;
SELECT pg_advisory_xact_lock(70123001);
ALTER TABLE accounts.users ADD COLUMN last_login_at timestamptz;
-- Preserve recorded history; NULL means no successful login is recorded.
UPDATE accounts.users u SET last_login_at = history.last_login_at
FROM (SELECT subject_id, max(occurred_at) AS last_login_at FROM accounts.audit
      WHERE action = 'login' GROUP BY subject_id) history
WHERE u.id = history.subject_id;
ALTER TABLE accounts.schema_version DROP CONSTRAINT schema_version_version_check;
UPDATE accounts.schema_version SET version = 3;
ALTER TABLE accounts.schema_version ADD CONSTRAINT schema_version_version_check CHECK (version = 3);
COMMIT;
