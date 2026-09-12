# Optional reservations and public account navigation

This increment supports one therapist and one exclusive room per appointment.
It includes signed-in customer booking, staff-entered guests, availability,
day/week staff agendas, rescheduling, cancellation, completion/no-show states,
and a customer's upcoming/history views. It is reusable backend/admin code;
Aloha-specific resource names and business rules stay in its example configuration.

## Try the example

Build with accounts and reservations enabled (the existing local cache already
has these flags). The content editor is independently optional:

```sh
cmake -S . -B Build -DWEBSITE_ENABLE_ACCOUNTS=ON -DWEBSITE_ENABLE_CONTENT_EDITOR=ON -DWEBSITE_ENABLE_RESERVATIONS=ON -DFELIS_USE_STD_FORMAT=OFF
cmake --build Build --parallel 2
python3 Examples/AlohaMassage/accounts_dev.py start
```

Stop the previous launcher before starting another. The launcher runs both
account and reservation migrations; it preserves existing users and sessions.
The public-only build and Python preview expose no login or reservation controls.

On [the public site](http://127.0.0.1:8082/?lang=bg), **Sign in / Вход** opens a
dialog using the existing account API and HttpOnly session cookie. Registration
and recovery links appear when enabled. After login, the header identifies the
signed-in user by display name, email and translated role. Everyone gets **My
profile** and Sign out; **My reservations** is customer-only. Administrators get
Administration; operators get that link when reservations are enabled. Customers
opening `/admin` are redirected to their profile. No credentials or tokens are
stored in localStorage. Window focus/page restoration refreshes account state.

Customers open [My reservations](http://127.0.0.1:8082/profile?lang=bg#section-reservations),
expand **Book an appointment**, choose service/duration/date/therapist, find a slot,
and confirm. Email verification is required for self-service booking; complete
it from My profile using the private local outbox. The server takes contact
details from that authenticated account. Upcoming and history views are separate;
choose a view and press Refresh. A cancelled future appointment appears in history.

Staff open [Reservations](http://127.0.0.1:8082/admin?lang=bg#section-reservations).
The **Book for a guest / Запишете гост** form accepts a guest's name and email or phone. It does not create
an account or link to an existing account by matching contact details. The calendar
supports a day or seven-day agenda and pagination. Choose the date/view and press
Refresh. Each confirmed appointment has appropriate management buttons. Reschedule
opens the booking form with the same service/duration and requires a fresh slot.
Completed/no-show states require an explicit staff action after the relevant time.
Finalized appointments cannot be reopened in this increment.

Staff profiles contain personal account settings without a self-booking section.
The API also rejects new self-bookings by admins/operators with 403, even if they
submit a request manually. Existing appointment records remain available in the
staff agenda; the own-history API remains readable after a customer's role changes.

Only the reservation section is available to operators; user management, content
publishing and newsletters retain their administrator-only permissions. Customers
can read their own history and cancel within the configured deadline. Customer
rescheduling is currently handled by staff.

**The Aloha configuration uses a demo therapist and demo room, daily 11:00–20:00.**
Only relaxation and deep-tissue massages are enabled for native booking initially.
There is a 15-minute cleanup buffer, a two-hour booking lead time, a 90-day booking
horizon, and a 24-hour customer cancellation deadline. Those are demonstration
policies, not a claim about the real studio's staff availability or cancellation terms.
The public booking page labels this as a local demo and keeps Fresha/phone options.
Local bookings are not synchronized to Fresha.

## Configuration and deployment

`custom_config.reservations.enabled` controls the module at runtime.
`WEBSITE_ENABLE_RESERVATIONS` controls compilation and defaults to OFF. Enabling
reservations requires accounts and a configured public service snapshot; customer
self-registration and the service editor are not required. With reservations
disabled, its routes and JavaScript asset are absent and no reservation schema
is needed. Enabling a module omitted from the binary fails startup.

The example configuration is the complete reference. Its fields are:

| Field | Meaning |
| --- | --- |
| `schema_file` | Initial migration path relative to the server configuration |
| `timezone` | PostgreSQL-supported operating zone; example `Europe/Sofia` |
| `slot_minutes` | 5–60 minutes, dividing 60; local slot grid |
| `lead_minutes` | 0–10,080 elapsed minutes before a new/moved appointment |
| `horizon_days` | 1–365 local calendar days ahead |
| `cancel_hours` | 0–168 elapsed hours of customer cancellation notice |
| `max_future_per_customer` | 1–50 active upcoming self-service bookings |
| `business_hours` | Weekly windows with ISO weekdays 1=Monday through 7=Sunday |
| `resources` | Stable IDs, kind `therapist`/`room`, localized names and weekly hours |
| `services` | Public service IDs, eligible therapist/room IDs and before/after buffers |
| `closures` | Local dates and resource IDs; empty resource list closes the whole business |

A weekly window is `{"days":[1,2,3,4,5,6,7],"start":"11:00","end":"20:00"}`.
Use separate windows for shifts with breaks. Every minute, including buffers,
must fit business and assigned-resource hours. Windows cannot span midnight;
the initial version does not offer appointments crossing the local date boundary.
Each service rule chooses exactly one eligible therapist and one eligible room.
Resources are exclusive: a room cannot host independent appointments simultaneously.
Never reuse an old resource ID for a different physical resource/person.

There is no schedule editor yet: stop the backend, edit the private configuration,
and restart. New resource IDs also require the reservation migration command.
Configuration updates affect new availability; existing appointments retain their
allocations and are not silently cancelled or moved. Review upcoming appointments
when changing schedules/closures. Removed resources remain in the database for
historical references. Public service text/prices stay in the public snapshot;
appointments store immutable title, duration, price/currency and contact snapshots.
Only priced duration variants from explicitly configured services are bookable.

For deployment, stop the old backend, back up the site database/configuration, and
run as the schema owner with the accounts database environment variable set:

```sh
./Build/WebSiteBackend --config=Examples/AlohaMassage/Config/server-accounts.json --migrate-accounts
./Build/WebSiteBackend --config=Examples/AlohaMassage/Config/server-accounts.json --migrate-reservations
```

The second command installs PostgreSQL's `btree_gist` extension in this site's
database. The PostgreSQL extension files must be available on that machine; they
are already present in the local extracted runtime. Normal startup performs no
DDL and checks the reservation schema/resources. Migrations are serialized and
repeatable. Reservations use schema version 2 and accounts use version 4. Runtime
grants supplement the existing account grants:

```sql
GRANT USAGE ON SCHEMA reservations TO aloha_app;
GRANT SELECT ON reservations.schema_version, reservations.resources TO aloha_app;
GRANT SELECT, INSERT, UPDATE ON reservations.appointments TO aloha_app;
GRANT SELECT, INSERT, UPDATE, DELETE ON reservations.occupancy TO aloha_app;
GRANT INSERT ON reservations.events TO aloha_app;
GRANT SELECT, INSERT ON reservations.reminders TO aloha_app;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA reservations TO aloha_app;
```

A whole-database `pg_dump` includes reservations, events and extension declarations.
Keep configuration/public content and private outbox backups alongside it, as
described in [ACCOUNTS.md](ACCOUNTS.md#disk-backups-and-a-fresh-test-database).

## API and consistency

All routes below require an authenticated enabled account and return private,
non-cacheable JSON. Writes reuse the same Origin/custom-header/CSRF protections
as other account operations. Work runs on the bounded account worker queue.

| Route | Purpose |
| --- | --- |
| `GET /api/v1/reservations/options` | Allowed services, current catalog revision, resource labels and date bounds |
| `GET /api/v1/reservations/availability` | Slots for `serviceId`, `durationMinutes`, `revision`, `date`, optional `therapistId` |
| `POST /api/v1/reservations` | Create a self-service booking or staff guest booking |
| `GET /api/v1/me/reservations?scope=upcoming&page=1` | Own appointments; scope is `upcoming` or `history`, 50/page |
| `GET /api/v1/admin/reservations?date=YYYY-MM-DD&days=7&page=1` | Staff-only agenda; 1–7 days, 50/page |
| `PATCH /api/v1/reservations` | Version-checked cancellation, staff rescheduling or final state |

Creation requires `serviceId`, integer `durationMinutes`, `revision`, local `date`,
UTC `startsAt` from availability, `therapistId` (empty = any), and `requestKey`.
Staff must include `guest: {name,email,phone,locale}`. Only customers can submit
without `guest`; customers cannot submit guest bookings. These role checks run
before request-key replay. No user ID or price is accepted.
The client retains the request key while retrying the same submission. The same
actor/key and identical body return the existing reservation; different contents
with a reused key return 409. A changed menu revision returns 409; use **Reload
booking options** before selecting a new slot.

PATCH takes string `id`, string `version`, and `action` (`cancelled`, `completed`,
`no_show`, or staff-only `reschedule`). Rescheduling additionally needs `date`,
`startsAt`, `therapistId`, and current `revision`; availability accepts staff-only
`excludeId` to ignore that appointment's current occupancy. Moving an appointment
preserves its originally agreed price. Missing/unauthorized customer records return
404; role violations return 403 and stale versions/slot conflicts return 409.

Authorization and SQL mutations run in one transaction. Availability is recomputed
before booking; both resource occupancy rows are inserted atomically. Half-open
timestamp ranges include buffers, and a GiST exclusion constraint independently
rejects overlapping occupancy for each resource. The current implementation shares
the existing per-site transaction lock: appropriate for small-business writes,
but not a claim of benchmarked high-volume booking throughput.

`Reservations::Module` composes `Configuration`, `Controller` and
`ReservationService`, registers routes, and retains shared ownership for route and
email callbacks. `Configuration` validates deployment settings and runs the existing
schema/resource provisioning checks. The controller owns endpoint dispatch, initial
request-shape checks and HTTP response/status construction. The service retains
business validation, authorization, idempotency and workflow sequencing.

HTTP work still runs inside the existing account dispatcher's authenticated
transaction. `ReservationRepository` borrows that call's database connection for
appointment reads, booking-contact lookup, request-key lookup, creation, schedule
and state updates, occupancy changes, events and invalidation of queued reminders.
It owns their SQL and row conversion without transaction control, reconnects or
retries. The service retains validation, authorization, replay decisions, version
and timing checks, page clamping, response assembly and the order of persistence
and notification calls. Rescheduling still replaces occupancy around the schedule
update while preserving the original service and price snapshot.

`ReservationCatalog` interprets each current public snapshot, filters bookable
variants and resolves offers. It hashes the original snapshot bytes for revisions
and does not cache a catalog across requests. `ReservationAvailability` calculates
slots from the PostgreSQL minute timeline and occupied ranges, applying the existing
lead time, buffers, schedules, closures and configured resource order. These concrete
components are owned by the service and borrow the same immutable settings.

The service retains request validation, revision checks, horizon gating, PostgreSQL
time/date and occupancy queries, row conversion and JSON response assembly. PostgreSQL
still defines local dates, UTC instants and daylight-saving transitions.

`ReservationNotifications` owns recipient selection, localized notice generation,
reminder scan orchestration and delivery-eligibility delegation. Its reservation-specific
SQL and row mapping live in `ReservationRepository`; queue insertion still uses the
existing `Accounts::QueueEmail` boundary. The service retains the explicit reminder-scan
transaction and advisory lock, and its existing compatibility entry points delegate to
the component. The component owns only the email-enabled flag, origin and UI settings it
needs, and borrows the immutable reservation settings; it retains no database connection.

Booking changes, queued-reminder invalidation, events and notices retain their original
order in the caller's transaction. Reminder jobs and their appointment/version records
commit together. Eligibility still runs inside the existing email worker's claim
transaction, which commits before delivery I/O. No schema migration is required.

Existing validation functions shared by configuration and request handling live
in `Source/Reservations/Validation.h`. Phone-character and decimal-identifier checks
continue to use `Source/InputValidation.h`; field-specific errors and their ordering
remain with each caller.

Instants are stored in UTC. Availability walks actual UTC minutes and checks each
minute against local schedules, including both DST transitions: nonexistent local
times are absent and repeated local times have distinct UTC values. The UI shows
the studio time zone/offset even when the visitor's device is in another zone.

Booking changes and confirmation jobs commit together when customer email is
enabled. Delivery failures retry independently and do not undo committed bookings.
Notifications are transactional, independent of newsletter consent. Account-linked
notices use the current verified email; staff guest notices use the supplied guest
address. The configured [email transport](CUSTOMER_EMAIL.md) supplies either a
private local outbox or Resend delivery with an administrator status panel.

### Appointment reminders

Set `reminders_enabled: true` and `reminder_hours: 24` inside the private reservations
configuration. Omitting `reminders_enabled` disables reminders; the lead time defaults
to 24 elapsed hours and accepts 1–168. The example enables 24-hour reminders in its
local outbox. Changes require a backend restart.

The email worker scans every 30 seconds, queues up to 20 due reminders per scan,
and records one job per appointment/version in `reservations.reminders`. A restart
cannot duplicate that version's reminder. The message uses the appointment's
language and the studio's configured time zone. Account-linked reminders use the
current verified email; disabled/unverified accounts are ineligible.

A reminder is due when a confirmed appointment enters the configured window and
still starts more than five minutes from now. Bookings created or rescheduled
inside that window receive their ordinary confirmation/change notice without an
immediate reminder. After downtime, eligible missed reminders are caught up until
that five-minute cutoff. This is an elapsed-hour window, including across daylight
saving changes, rather than “the same clock time yesterday.”

Cancellation and rescheduling discard queued reminders. A rescheduled version can
receive its own later reminder. Delivery rechecks the version, state, future start,
current recipient and enabled configuration; already claimed messages may finish
while a change occurs. Reminders expire at the appointment start. The email module
being disabled pauses its entire queue; disabling reminders or reservations causes
its existing reminders to be skipped when the email worker next examines them.

`--migrate-reservations` upgrades schema version 1 to version 2 using
`Migrations/Reservations/002_reminders.sql`. Run it after the accounts migration.
The runtime role additionally needs:

```sql
GRANT SELECT, INSERT ON reservations.reminders TO aloha_app;
```

The table uses existing appointment/mail-job IDs and has no new sequence.

## Verification and remaining work

```sh
python3 Tests/accounts_integration.py --reservations --customer-email --email-delivery --content-editor
python3 Tests/accounts_integration.py
```

The isolated tests cover identity/role/CSRF checks, server-side pricing, duplicate
submissions, concurrent collisions, the exclusion constraint, buffers, guest
separation, rescheduling rollback, versions, cancellation/history, explicit
completion, persistence, closures and daylight-saving transitions. Existing email,
editor, account and disabled-module tests remain part of the same suite.

Both C++ build configurations (reservations enabled and public-only) and the
combined integration suite passed on 10 September 2026. The accounts-only suite
also passed with reservations disabled. Browser checks using an isolated database
covered public sign-in in Bulgarian/English, customer contact prefill and booking,
staff agenda/rescheduling, and mobile header/overflow checks. Browser cancellation
verification stopped at the native confirmation dialog because browser automation
could not operate it; cancellation and history were verified through the real API tests.

Anonymous self-service booking, guest-account claiming, multi-therapist/couples
appointments, capacity sharing, deposits/payments, external calendar
sync, loyalty and aggregate statistics are not implemented in this increment.
