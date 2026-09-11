BEGIN;
SELECT pg_advisory_xact_lock(70123001);
CREATE SCHEMA IF NOT EXISTS accounts;
CREATE TABLE IF NOT EXISTS accounts.schema_version (
    version integer PRIMARY KEY CHECK (version = 1)
);
INSERT INTO accounts.schema_version VALUES (1) ON CONFLICT DO NOTHING;
CREATE TABLE IF NOT EXISTS accounts.users (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    email text NOT NULL UNIQUE CHECK (email = lower(email) AND length(email) <= 254),
    password_hash text NOT NULL,
    display_name text NOT NULL,
    phone text NOT NULL DEFAULT '',
    locale text NOT NULL,
    role text NOT NULL CHECK (role IN ('admin', 'operator', 'customer')),
    enabled boolean NOT NULL DEFAULT true,
    version bigint NOT NULL DEFAULT 1,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE IF NOT EXISTS accounts.sessions (
    token_hash text PRIMARY KEY,
    user_id bigint NOT NULL REFERENCES accounts.users(id),
    csrf_token text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    last_seen timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz NOT NULL
);
CREATE INDEX IF NOT EXISTS sessions_user ON accounts.sessions(user_id);
CREATE INDEX IF NOT EXISTS sessions_expiry ON accounts.sessions(expires_at);
CREATE TABLE IF NOT EXISTS accounts.audit (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    actor_id bigint REFERENCES accounts.users(id),
    subject_id bigint REFERENCES accounts.users(id),
    action text NOT NULL,
    occurred_at timestamptz NOT NULL DEFAULT now()
);
COMMIT;
