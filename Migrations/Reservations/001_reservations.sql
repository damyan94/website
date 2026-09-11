BEGIN;
SELECT pg_advisory_xact_lock(70123003);
CREATE EXTENSION IF NOT EXISTS btree_gist;
CREATE SCHEMA reservations;
CREATE TABLE reservations.schema_version(version integer PRIMARY KEY CHECK (version = 1));
INSERT INTO reservations.schema_version VALUES (1);
CREATE TABLE reservations.resources (
    id text PRIMARY KEY,
    kind text NOT NULL CHECK (kind IN ('therapist','room'))
);
CREATE TABLE reservations.appointments (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    actor_id bigint NOT NULL REFERENCES accounts.users(id),
    request_key text NOT NULL,
    request_hash text NOT NULL,
    user_id bigint REFERENCES accounts.users(id),
    contact_name text NOT NULL,
    contact_email text NOT NULL,
    contact_phone text NOT NULL,
    locale text NOT NULL,
    service_id text NOT NULL,
    service_title jsonb NOT NULL,
    duration_minutes integer NOT NULL CHECK (duration_minutes BETWEEN 5 AND 240),
    price_minor bigint NOT NULL CHECK (price_minor >= 0),
    currency text NOT NULL,
    starts_at timestamptz NOT NULL,
    ends_at timestamptz NOT NULL CHECK (ends_at > starts_at),
    therapist_id text NOT NULL REFERENCES reservations.resources(id),
    room_id text NOT NULL REFERENCES reservations.resources(id),
    buffer_before integer NOT NULL CHECK (buffer_before BETWEEN 0 AND 60),
    buffer_after integer NOT NULL CHECK (buffer_after BETWEEN 0 AND 60),
    state text NOT NULL DEFAULT 'confirmed' CHECK (state IN ('confirmed','cancelled','completed','no_show')),
    version bigint NOT NULL DEFAULT 1,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE(actor_id,request_key)
);
CREATE INDEX appointments_customer ON reservations.appointments(user_id, starts_at, id);
CREATE INDEX appointments_calendar ON reservations.appointments(starts_at, id);
-- Every exclusive therapist and room has an occupancy row. A single transaction
-- acquires both; PostgreSQL rejects conflicts even from another server process.
CREATE TABLE reservations.occupancy (
    appointment_id bigint NOT NULL REFERENCES reservations.appointments(id),
    resource_id text NOT NULL REFERENCES reservations.resources(id),
    during tstzrange NOT NULL CHECK (NOT isempty(during) AND NOT lower_inf(during) AND NOT upper_inf(during)),
    PRIMARY KEY (appointment_id, resource_id),
    EXCLUDE USING gist (resource_id WITH =, during WITH &&)
);
CREATE TABLE reservations.events (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    appointment_id bigint NOT NULL REFERENCES reservations.appointments(id),
    actor_id bigint NOT NULL REFERENCES accounts.users(id),
    action text NOT NULL,
    occurred_at timestamptz NOT NULL DEFAULT now()
);
COMMIT;
