BEGIN;
SELECT pg_advisory_xact_lock(70123001);
ALTER TABLE accounts.mail_jobs DROP CONSTRAINT mail_jobs_state_check;
ALTER TABLE accounts.mail_jobs ADD CONSTRAINT mail_jobs_state_check CHECK
    (state IN ('queued','processing','accepted','delivered','delayed','bounced','complained','skipped','failed'));
ALTER TABLE accounts.mail_jobs DROP CONSTRAINT mail_jobs_kind_check;
ALTER TABLE accounts.mail_jobs ADD CONSTRAINT mail_jobs_kind_check CHECK
    (kind IN ('challenge','notice','newsletter','reminder'));
ALTER TABLE accounts.mail_jobs ADD COLUMN transport text CHECK (transport IN ('local_outbox','resend'));
ALTER TABLE accounts.mail_jobs ADD COLUMN delivery_key text UNIQUE;
ALTER TABLE accounts.mail_jobs ADD COLUMN provider_id text UNIQUE;
ALTER TABLE accounts.mail_jobs ADD COLUMN first_attempt_at timestamptz;
ALTER TABLE accounts.mail_jobs ADD COLUMN last_error text NOT NULL DEFAULT '';
ALTER TABLE accounts.mail_jobs ADD COLUMN status_code integer;
ALTER TABLE accounts.mail_jobs ADD COLUMN request_body text;
-- Previously attempted jobs belong to the local transport, including its retries.
UPDATE accounts.mail_jobs SET transport='local_outbox' WHERE attempts>0 OR state='delivered';
CREATE INDEX mail_jobs_status ON accounts.mail_jobs(state,id DESC);
CREATE TABLE accounts.mail_events (
    event_id text PRIMARY KEY,
    provider_id text NOT NULL,
    event_type text NOT NULL,
    occurred_at timestamptz NOT NULL,
    received_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX mail_events_provider ON accounts.mail_events(provider_id);
CREATE TABLE accounts.mail_suppressions (
    recipient text PRIMARY KEY,
    reason text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE accounts.mail_worker_status (
    singleton boolean PRIMARY KEY CHECK (singleton),
    heartbeat_at timestamptz,
    last_error text NOT NULL DEFAULT '',
    pause_until timestamptz NOT NULL DEFAULT now()
);
INSERT INTO accounts.mail_worker_status(singleton) VALUES(true);
ALTER TABLE accounts.schema_version DROP CONSTRAINT schema_version_version_check;
UPDATE accounts.schema_version SET version=4;
ALTER TABLE accounts.schema_version ADD CONSTRAINT schema_version_version_check CHECK(version=4);
COMMIT;
