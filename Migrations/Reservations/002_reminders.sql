BEGIN;
SELECT pg_advisory_xact_lock(70123003);
CREATE TABLE reservations.reminders (
    appointment_id bigint NOT NULL REFERENCES reservations.appointments(id),
    appointment_version bigint NOT NULL,
    mail_job_id bigint NOT NULL UNIQUE REFERENCES accounts.mail_jobs(id),
    PRIMARY KEY (appointment_id, appointment_version)
);
ALTER TABLE reservations.schema_version DROP CONSTRAINT schema_version_version_check;
UPDATE reservations.schema_version SET version=2;
ALTER TABLE reservations.schema_version ADD CONSTRAINT schema_version_version_check CHECK(version=2);
COMMIT;
