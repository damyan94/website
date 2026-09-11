BEGIN;
SELECT pg_advisory_xact_lock(70123001);
ALTER TABLE accounts.users ADD COLUMN email_verified_at timestamptz;
ALTER TABLE accounts.users ADD COLUMN credential_version bigint NOT NULL DEFAULT 1;
-- Social-only credentials can be introduced without changing the permanent user ID.
ALTER TABLE accounts.users ALTER COLUMN password_hash DROP NOT NULL;
CREATE TABLE accounts.login_identities (
    user_id bigint NOT NULL REFERENCES accounts.users(id),
    provider text NOT NULL,
    issuer text NOT NULL,
    subject text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (provider, issuer, subject),
    UNIQUE (user_id, provider, issuer)
);
CREATE TABLE accounts.email_challenges (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    token_hash text NOT NULL UNIQUE CHECK (length(token_hash) = 64),
    purpose text NOT NULL CHECK (purpose IN ('register','verify','reset','change','subscribe')),
    user_id bigint REFERENCES accounts.users(id),
    credential_version bigint,
    email text NOT NULL,
    locale text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz NOT NULL DEFAULT now() + interval '20 minutes',
    consumed_at timestamptz
);
CREATE INDEX email_challenges_address ON accounts.email_challenges(email, created_at);
CREATE TABLE accounts.subscriptions (
    user_id bigint PRIMARY KEY REFERENCES accounts.users(id),
    email text NOT NULL,
    confirmed_at timestamptz,
    generation bigint NOT NULL DEFAULT 1
);
CREATE TABLE accounts.subscription_events (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id bigint NOT NULL REFERENCES accounts.users(id),
    email text NOT NULL,
    action text NOT NULL,
    source text NOT NULL,
    occurred_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE accounts.newsletter_links (
    token_hash text PRIMARY KEY CHECK (length(token_hash) = 64),
    user_id bigint NOT NULL REFERENCES accounts.users(id),
    generation bigint NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE accounts.campaigns (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    request_key text NOT NULL UNIQUE,
    actor_id bigint NOT NULL REFERENCES accounts.users(id),
    locale text NOT NULL,
    subject text NOT NULL,
    body text NOT NULL,
    state text NOT NULL DEFAULT 'draft' CHECK (state IN ('draft','queued')),
    created_at timestamptz NOT NULL DEFAULT now(),
    queued_at timestamptz
);
CREATE TABLE accounts.mail_jobs (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    kind text NOT NULL CHECK (kind IN ('challenge','notice','newsletter')),
    user_id bigint REFERENCES accounts.users(id),
    challenge_id bigint REFERENCES accounts.email_challenges(id) ON DELETE SET NULL,
    campaign_id bigint REFERENCES accounts.campaigns(id),
    subscription_generation bigint,
    recipient text NOT NULL,
    subject text NOT NULL,
    body text NOT NULL,
    unsubscribe_token text,
    state text NOT NULL DEFAULT 'queued' CHECK (state IN ('queued','processing','delivered','skipped','failed')),
    attempts integer NOT NULL DEFAULT 0,
    available_at timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    finished_at timestamptz,
    UNIQUE (campaign_id, user_id)
);
CREATE INDEX mail_jobs_ready ON accounts.mail_jobs(available_at, id) WHERE state IN ('queued','processing');
ALTER TABLE accounts.schema_version DROP CONSTRAINT schema_version_version_check;
UPDATE accounts.schema_version SET version = 2;
ALTER TABLE accounts.schema_version ADD CONSTRAINT schema_version_version_check CHECK (version = 2);
COMMIT;
