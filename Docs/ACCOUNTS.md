# Optional accounts module

Implemented: administrator-created accounts, login/logout, own-profile editing,
password changes, role management, account disabling and audit records.
The optional [service editor](CONTENT_EDITOR.md) now adds admin content forms.
Optional [customer email and newsletters](CUSTOMER_EMAIL.md) add self-registration,
verification, recovery, verified email changes and campaigns with either a private
local outbox or Resend delivery. Optional [reservations](RESERVATIONS.md) add customer booking/history and
staff appointment management. Live domain/hosting setup and statistics remain
future increments.

There are no default credentials. Accounts and sessions live in PostgreSQL;
never put private data in `Content/site.json` or `Public`.

## Local setup

The accounts-enabled C++ binary is in `Build`. On a fresh machine, review
`./Scripts/install-deps.sh --dry-run`, then run `./Scripts/install-deps.sh` for apt
prerequisites. Drogon must be installed separately when unavailable from apt, and
the Felis submodule must already be populated; see the root
[development commands](../README.md#development-commands).

This checkout also has PostgreSQL 15 packages extracted into `Build/LocalPostgres`.
The existing CMake cache uses those libraries. The launcher prefers those local
binaries, falling back to the system PostgreSQL tools from `pg_config --bindir`
when they are absent. Keep the PostgreSQL major version compatible with an existing
data directory. The launcher manages a private cluster and does not use a system
PostgreSQL service.

From the repository root:

```sh
./Scripts/build.sh
./Scripts/run.sh
```

In another terminal, replace the example email with your own:

```sh
./Scripts/run.sh bootstrap your@email.com
```

Enter and repeat a password at the hidden prompt. Use 15–128 characters;
passwords are not trimmed and control characters are rejected. Email addresses
are case insensitive; this version supports ordinary ASCII email addresses.
Bootstrap refuses once an administrator already exists.

Open [administration](http://127.0.0.1:8082/admin?lang=bg) or
[your profile](http://127.0.0.1:8082/profile?lang=bg). Use `lang=en` for English.
Admins can create accounts and change roles/access. Users can edit their own
name, phone and preferred language and change their password. With customer email
enabled, they can also verify/change their address and manage newsletter consent.
The login form links to registration and password recovery.

For a forgotten password, the machine owner can run:

```sh
./Scripts/run.sh reset-password their@email.com
```

This requires local database access and revokes every session for that account.
It does not re-enable a disabled account; an administrator can do that in the UI.

The launcher uses `Examples/AlohaMassage/Runtime/Accounts` with private directory
permissions. PostgreSQL listens only on its private Unix socket, with TCP disabled.
Trust authentication is used only for this local development socket. The demo
role owns its database; production should separate migration and runtime roles.
Ctrl+C stops processes started by the launcher and preserves data for the next run.
Provisioning from a second terminal reuses the running database without stopping it.
Keep `Runtime`, database backups, secret environment files and local build/package
directories out of version control and public web roots.

Within the Aloha example, `Config/server.json` remains public-only on port 8080.
The separate `Config/server-accounts.json` enables accounts, the service editor,
customer email with a private local outbox, and reservations on port 8082.
`Scripts/build.sh` enables all three build options; `Scripts/run.sh` selects that
full configuration by default. Use `Scripts/run.sh public` for the public-only
build, or `Scripts/run.sh preview` for the Python frontend preview without
authentication. These commands wrap the existing `accounts_dev.py` and `preview.py`
launchers, which remain directly usable.

## Build-time and runtime modularity

Raw CMake defaults to accounts disabled. The build script explicitly selects each
module; the public-only variant is:

```sh
./Scripts/build.sh --public-only
```

That build does not discover/link PostgreSQL or Argon2 and contains no accounts
code, routes or UI. The format option is the existing compiler compatibility
setting for this machine; see the root README for other environments.

With PostgreSQL, Argon2 and OpenSSL development files installed:

```sh
./Scripts/build.sh
```

For the locally extracted PostgreSQL, these paths are already in the current
build cache. The build script also detects them when configuring a new full build
cache in this checkout. The equivalent manual configuration is:

```sh
cmake -S . -B Build -DWEBSITE_ENABLE_ACCOUNTS=ON -DWEBSITE_ENABLE_CONTENT_EDITOR=ON -DWEBSITE_ENABLE_RESERVATIONS=ON -DFELIS_USE_STD_FORMAT=OFF \
  -DPostgreSQL_INCLUDE_DIR="$PWD/Build/LocalPostgres/root/usr/include/postgresql" \
  -DPostgreSQL_LIBRARY="$PWD/Build/LocalPostgres/root/usr/lib/x86_64-linux-gnu/libpq.so"
cmake --build Build --parallel 2
```

Authentication is not trivial. We use the Argon2 reference library for hashing,
PostgreSQL's libpq for parameterized queries, and the existing OpenSSL dependency
for secure random tokens, token digests and constant-time comparisons. No custom
cryptographic primitives or authentication protocol are implemented. Drogon's
installed build has no database drivers, so libpq avoids rebuilding it.

An external identity provider remains an alternative for MFA, passkeys, social
login and recovery. It adds integration/deployment work and does not replace our
application permissions. This increment uses local accounts with focused tests;
it is not an independently audited identity product.

Even an accounts-capable binary enables the module only through `custom_config`:

```json
{
  "accounts": {
    "enabled": true,
    "database_url_env": "ALOHA_DATABASE_URL",
    "public_origin": "https://example.com",
    "ui_root": "../../../Admin",
    "schema_file": "../../../Migrations/001_accounts.sql",
    "idle_seconds": 3600,
    "absolute_seconds": 43200,
    "locales": ["bg", "en"]
  }
}
```

Paths resolve relative to the configuration file. The origin is one canonical
scheme/host/optional-port without a trailing slash. nginx must preserve that Host.
Supply the database connection string through the named protected environment
variable or libpq service/password-file setup. Never include passwords in public
content, command arguments or logs. For remote PostgreSQL use certificate-verified
TLS, such as libpq `sslmode=verify-full`.

After setting the database environment using migration/owner credentials:

```sh
./Build/WebSiteBackend --config=Examples/AlohaMassage/Config/server-accounts.json --migrate-accounts
./Build/WebSiteBackend --config=Examples/AlohaMassage/Config/server-accounts.json --bootstrap-admin=your@email.com
```

These commands exit without starting HTTP. Migration is transactional and
repeatable through schema version 4, including upgrading versions 1–3 while retaining
existing accounts and sessions. Migration `003_user_activity.sql` adds a nullable
last-successful-login timestamp and backfills it from retained login audit records.
It lives beside `001_accounts.sql` by default; `activity_schema_file` can override
its path relative to the configuration file. Stop the old backend before applying the migration. Normal
startup checks the schema but runs no DDL. The local owner command
`--reset-account-password=EMAIL` revokes sessions. `--password-stdin` is an explicit
provisioning/test option; use the hidden prompt interactively.

For production, create a separate runtime database role. Besides connecting to its
own database, it needs these grants from the schema owner:

```sql
GRANT USAGE ON SCHEMA accounts TO aloha_app;
GRANT SELECT ON accounts.schema_version TO aloha_app;
GRANT SELECT, INSERT, UPDATE ON accounts.users TO aloha_app;
GRANT SELECT, INSERT, UPDATE, DELETE ON accounts.sessions TO aloha_app;
GRANT INSERT ON accounts.audit TO aloha_app;
GRANT SELECT, INSERT, UPDATE ON accounts.email_challenges,
  accounts.subscriptions, accounts.subscription_events,
  accounts.newsletter_links, accounts.campaigns, accounts.mail_jobs,
  accounts.mail_events, accounts.mail_suppressions, accounts.mail_worker_status TO aloha_app;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA accounts TO aloha_app;
```

Each website should have its own database, credentials and process. An enabled
but misconfigured module fails startup, including unavailable database/schema/UI
resources. It never falls back to permissive access. Omitting accounts or setting
`enabled: false` needs none of those resources.

## Reusing the administration interface

The same `Admin` directory serves every accounts-enabled site. The example now
sets presentation through `custom_config.accounts.ui`:

```json
"ui": {
  "site_name": "Aloha Massage",
  "default_locale": "bg",
  "support_text": {
    "bg": "За помощ с профила се свържете със студиото: +359 879 925 093.",
    "en": "For account help, contact the studio: +359 879 925 093."
  }
}
```

For another business, change those values in its own server configuration and
restart its process. These settings are deployment-owned; the admin panel does not
edit configuration or permissions. Omitting `ui` gives a neutral title, English
interface and generic support wording. Individual fields are optional. Site names
are limited to 100 characters and each support translation to 500; control characters,
unknown fields and unsupported interface locales fail startup.

`GET /api/v1/auth/options` retains its existing fields and adds a public `ui` object:
`siteName`, `defaultLocale`, and `supportText`. Only those approved presentation
fields are copied; database settings, private paths and other server configuration
are never exposed. Use public contact information here. Values render as plain text.
The name/support information also accompanies account verification/recovery emails;
newsletter sender/footer remain separately configurable under `customer_email`.

Interface translations live in `Admin/i18n.js`, grouped by feature. BG/EN are the
currently supported interface languages; `?lang=bg` or `?lang=en` overrides the
configured default. Adding another interface language requires translating its
shared dictionaries and extending the backend's supported-interface-locale check.
Website content languages remain independent: the service editor uses the content
schema's languages, while profile/registration/newsletter selectors use
`accounts.locales`. Neither requires the site to offer both BG and EN. Account
emails use BG for Bulgarian recipients and English otherwise in this increment.

The shared page displays one enabled section at a time, using the same admin
frontend for each business. Navigation, language/sign-out controls and status
messages stay in a sticky ribbon while scrolling. Section links use `#section-ID`;
deep links and browser Back/Forward select the corresponding panel. Missing or
forbidden sections fall back to the first available section. The navigation
scrolls horizontally on narrow screens.

| File | Responsibility |
| --- | --- |
| `Admin/admin.js` | Sign-in/out, initial options/profile loading, shared status and interface language |
| `Admin/public-account.js` | Reusable public-header sign-in dialog, identity and role-aware navigation |
| `Admin/modules.js` | Built-in section registry, selected panel, URL history, loading, retries and cleanup |
| `Admin/profile.js` | Profile, password, verified email and subscription forms |
| `Admin/users.js` | Searchable/paginated user table, account creation, roles and access |
| `Admin/content.js` | Service editor and its specific validation/display rules |
| `Admin/newsletters.js` | Campaign preview, queueing and status |
| `Admin/i18n.js` | Shared interface dictionaries, locale selection and public presentation |
| `Admin/index.html` | Shared layout and inert profile/user form templates |

A module has an ID, translated navigation title, visibility predicate and a factory
returning `load()`, `dispose()` and optionally `update(profile)`. Factories receive
an isolated container and shared API/status callbacks. Existing module instances
survive section switches and profile refreshes, preserving service/newsletter drafts. Sections
are loaded once at sign-in and hidden when unselected. Logout, a different
signed-in user or a removed section disposes its instance. A section load failure
shows a local retry control while other sections can continue; authentication/access
failures return to the shared error handling.

Add future products/reports by adding a focused module to the registry
and registering its shipped JavaScript asset through the backend. Keep backend
permissions and input validation on every endpoint. Configuration never supplies
arbitrary script URLs. Visibility is only presentation and cannot grant privileges.
Products can have their own stock/variant rules without changing the service editor.
No generic schema/form engine or new frontend dependency has been introduced.

Disabling newsletters/content editing removes those admin sections. `/profile`
shows personal settings and, for customers when enabled, their own reservations.
Admins/operators use the staff reservations panel for guest bookings and management;
they cannot self-book. Customers opening `/admin` are redirected to `/profile`.
The public header and sticky account ribbon show the signed-in name/email/role and
clear the label on logout. Returning to a signed-in account page checks for session
or role changes; unchanged sessions preserve editor drafts.
Operators can use the staff reservations section; user management, newsletters and
service editing require an administrator. A website with staff administration still requires accounts,
but public customer registration can be disabled independently. Compiling out or
disabling the whole accounts module removes its APIs and admin assets.

Validation includes module visibility, draft preservation, failure/retry and cleanup,
translation coverage, independent content languages, invalid configuration and a
second isolated business/database serving identical admin assets. Existing account,
newsletter and content-editor integration checks remain in the same test command.
The module tests use a small DOM stand-in. No new frontend dependency is required.

In **Users**, enter part of a name/email, choose role/access and sort order, then
press **Apply filters**. **Clear filters** restores defaults; **Refresh** retains
the applied filters and page. Search, filtering and sorting cover the whole database,
with 25 accounts per page. Rows include creation and last-login dates, verification
status, and role/access controls. Dates use the browser's time zone; hovering a date
shows its UTC value. Last login means successful authentication, not online presence.
“No recorded login” can also mean historical audit data was unavailable. Activity
updates do not invalidate concurrent profile edits. Use the expandable **Create an
account** form above the table to provision another account. Saving role/access
changes retains the current filters, so the changed account may leave the results.
Use **My profile** for your own personal details and password.

## API contracts

| Method and route | Access and purpose |
| --- | --- |
| `GET /api/v1/features` | Public; reports whether accounts and content editing are enabled |
| `GET /api/v1/auth/options` | Public email flags, account locales and safe admin presentation settings |
| `POST /api/v1/auth/login` | Public, throttled; creates a session |
| `POST /api/v1/auth/logout` | Signed in; revokes current session |
| `GET /api/v1/me` | Own profile, CSRF token and supported languages |
| `PATCH /api/v1/me` | Own name/phone/language with a version check |
| `POST /api/v1/me/password` | Verifies current password; changes it and revokes all sessions |
| `GET /api/v1/admin/users?after=ID` | Admin; up to 50 accounts and a next-page cursor |
| `GET /api/v1/admin/users?page=1&q=example&role=customer&enabled=true&sort=createdAt&order=desc` | Admin; filtered table, 25 accounts per page and totals |
| `POST /api/v1/admin/users` | Admin; creates account with initial password and role |
| `PATCH /api/v1/admin/users` | Admin; changes role/enabled state with a version check |

The paginated user list accepts `q` (literal name/email substring, up to 100
characters), `role` (`admin`, `operator`, `customer` or empty), `enabled` (`true`,
`false` or empty), `sort` (`createdAt`, `lastLoginAt`, `email`) and `order` (`asc`,
`desc`). Defaults are page 1, creation date descending, and no filters. It returns
`users`, `page`, `pages`, `pageSize` and `total`. Page numbers are limited to
1–1,000,000 and clamped to the last available page. Equal sort values use the user
ID as a tie-breaker; missing login dates sort last in either direction. A changing
dataset can shift records between pages; Refresh updates the current view.
Without these query parameters the existing 50-row `after`/`nextCursor` contract
is retained. Do not mix cursor and page parameters. Both responses add `createdAt`
and nullable `lastLoginAt` as UTC ISO timestamps, plus the `emailVerified` boolean.

Operators/customers have their own profile access. When reservations are enabled,
operators additionally manage appointments through its staff section; customer
history remains scoped to their own account. Allowed JSON fields
are explicit; own-profile updates reject browser-supplied IDs and roles. IDs and
versions are JSON strings. Account deletion is not implemented: disabling access
preserves records needed by future bookings and audit.

The admin HTML/JS are public presentation assets containing no credentials or
user records. Every private API operation checks the live account and session.
Changing the page or calling an endpoint directly cannot grant permissions.

## Security and limits

- Argon2id uses a random salt, 19 MiB memory, two iterations and one lane, following
  the [OWASP baseline](https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html).
- Session tokens contain 256 random bits; only SHA-256 token digests are stored.
  Hashes/digests never appear in the API. Production cookies are Secure, HttpOnly,
  SameSite=Lax, have Path=/ and a `__Host-` prefix without Domain. Cookie names
  depend on origin, avoiding local port collisions. See
  [OWASP session guidance](https://cheatsheetseries.owasp.org/cheatsheets/Session_Management_Cheat_Sheet.html).
- Account writes require JSON, exact Origin and a custom request header. The
  optional newsletter one-click endpoint instead requires a per-recipient random
  capability and the explicit unsubscribe POST form; it cannot change account access. Authenticated
  writes also require a session-bound CSRF token. Login has origin/header protection
  before a session exists. Session cookies are never readable by JavaScript;
  CSRF tokens stay in memory, not localStorage.
- HTTP requires explicit `allow_insecure_loopback: true`, a `127.0.0.1` origin and
  loopback listeners. Production needs HTTPS at the proxy and an isolated backend
  listener. Forwarded headers are not blindly trusted.
- Password work limits per minute: 120 globally, 60 per direct peer, 10 per login
  email or authenticated session for other password operations. The bounded
  in-memory limiter resets on restart. Behind nginx, add client-IP limits there;
  the backend sees the proxy as its direct peer.
- Two workers and a 64-request queue keep hashing/database calls off Drogon's HTTP
  event loop. Connections/statements have timeouts. Queries use separate SQL
  parameters through [libpq](https://www.postgresql.org/docs/current/libpq-exec.html).
- Account transactions serialize using a per-database lock for consistent
  permissions, revocation and administrator protection. This is a simple starting
  point for small sites, not a high-throughput identity server. More granular
  locking should follow measured demand.
- Database errors do not expose SQL or credentials. Subsequent requests can
  reconnect; uncertain writes are never replayed automatically. Reload state
  before retrying a failed or timed-out mutation.
- Version checks prevent lost updates. Password/access changes revoke sessions
  transactionally. Admins cannot disable/demote themselves, and concurrent changes
  cannot remove the last active administrator. Audit records contain actor,
  subject, action and timestamp, without copied credentials or contact details.

Back up and test restoring PostgreSQL. Establish retention before production.
Staff MFA and breached-password screening remain
follow-up work. Registration/recovery/verified email changes now use the optional
[customer email module](CUSTOMER_EMAIL.md). The UI is Bulgarian/English; backend error
messages are currently English.

## Verification and extension

```sh
python3 Tests/accounts_integration.py
```

The test starts an isolated PostgreSQL cluster with temporary data under `Build`.
It needs PostgreSQL binaries and both accounts-enabled `Build/WebSiteBackend` and
compiled-out `Build/PublicOnly/WebSiteBackend`. It checks authentication, roles,
ownership, CSRF/origin/JSON validation, Unicode profiles, stale edits, disabling,
password changes/recovery, session expiry/revocation/persistence, pagination,
audit, concurrent administrator changes, throttling, cookie policy, private paths
database-outage recovery and optional-module behaviour. It sends no email.
The full suite (`--reservations --customer-email --content-editor`) also checks
the shared public-login controller with delayed responses, role navigation,
interface fallback and disposal. The refactor was checked in the browser for
customer/admin login, profile/admin destinations, configured branding, three
content languages, public-only behavior and mobile navigation.

Public sites can import `mountPublicAccount` from `/accounts-assets/public-account.js`
after checking `/api/v1/features`. Pass `navigation` and `dialog` DOM elements,
optional `profileLink`, `features`, `language` and an optional `onChange(user)` callback.
The returned controller supports `setLanguage(locale)`, `refresh()` and `dispose()`.
It uses the shared account API/translations and has no business-specific text or
assets. Reuse the example's account CSS classes or style them for another site.
The module is unavailable when accounts are disabled; public rendering remains independent.

Frontend code uses a small `AccountApi` class because it owns a CSRF token, and a
public-header controller owns dialog/session lifecycle. Other page rendering and
event handlers remain functions. Dynamic values use `textContent`.
No bundler, framework or class hierarchy was added. The separately configurable
service editor adds content forms behind these permissions and keeps JSON as
content storage. Accounts-only sites can disable that module independently.

Account request transport is split into parsing, throttling, queue admission and
execution helpers. `AccountStore::Handle` owns authorization and the transaction
for authenticated actions; operation helpers do not commit independently. Public
email challenge completion validates/locks the token, applies its purpose-specific
operation and consumes it within one transaction. Field-set validation and session
revocation are shared within accounts; phone/identifier syntax also uses the small
reusable `InputValidation` helpers. Application configuration has separate validation
and optional-module setup methods. No database migration is needed for this refactor.

## Disk, backups and a fresh test database

These instructions describe the current local demo. They do not configure automatic
backups or change/delete the existing database.

**Capacity estimate:** 1,000 accounts and five reservations/day mean about 1,825
reservations/year. Assuming short text fields, a few related records/indexes per
booking and no binary attachments, the core account/booking data should be in the
tens of MB. A reasonable initial planning allowance is **100–300 MB for the active
application database** with moderate activity. This is an estimate, not a measured
year of reservation activity. Audit/login
volume, newsletter frequency and retention can increase it substantially.

On 9 September 2026 the current local PostgreSQL **whole cluster** occupied about
47 MiB on disk, and Aloha's public assets about 2 MiB. These measurements are not a
per-user growth rate. PostgreSQL also needs transaction-log (WAL), temporary and
maintenance space outside the application tables. Initially allow **5–10 GB for
database/runtime working space per small site**, excluding OS/build files and backup
retention, then monitor actual growth. This is headroom, not expected annual usage.

Newsletter/outbox storage is separate: 52 weekly campaigns to 1,000 subscribers
produce 52,000 messages; at an assumed 5 KB per JSON message, that is roughly
260 MB/year in the private outbox, plus database job metadata. Actual message
length varies. PDFs and images would add their own storage. Mail/audit history has
no automatic retention cleanup yet. Content backups stop accepting further changed
saves at 512 files until older backups are archived.

Measure the application database through SQL, and the whole runtime using `du`:

```sql
SELECT pg_size_pretty(pg_database_size('aloha'));
```

```sh
du -sh Examples/AlohaMassage/Runtime/Accounts/pgdata Examples/AlohaMassage/Runtime/Email Examples/AlohaMassage/Runtime/ContentBackups
```

`pg_database_size` measures the database, not the whole cluster/WAL or external
files. See [PostgreSQL size functions](https://www.postgresql.org/docs/15/functions-admin.html#FUNCTIONS-ADMIN-DBSIZE).

### Manual backup now

Use `pg_dump` to make a consistent logical backup while PostgreSQL is running.
Do not copy its live data directory with a normal file-copy command. An ordinary
filesystem backup requires PostgreSQL to be fully stopped and must include the
whole cluster and any external WAL/tablespaces. Restoring physical files also needs
a compatible PostgreSQL installation. See
[SQL backups](https://www.postgresql.org/docs/15/backup-dump.html) and
[filesystem backups](https://www.postgresql.org/docs/15/backup-file.html).

For the locally extracted PostgreSQL on this machine, with the demo running:

```sh
cd /home/damyan.damyanov/repos/personal/WebSite_1
umask 077
ALOHA_PG_BIN="$PWD/Build/LocalPostgres/root/usr/lib/postgresql/15/bin"
ALOHA_PG_SOCKET="$PWD/Examples/AlohaMassage/Runtime/Accounts/socket"
export LD_LIBRARY_PATH="$PWD/Build/LocalPostgres/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$HOME/aloha-backups"
chmod 700 "$HOME/aloha-backups"
ALOHA_DUMP="$HOME/aloha-backups/aloha-$(date +%Y%m%d-%H%M%S).dump"
"$ALOHA_PG_BIN/pg_dump" -h "$ALOHA_PG_SOCKET" -p 55432 -U aloha_dev -d aloha -Fc -f "$ALOHA_DUMP" &&
"$ALOHA_PG_BIN/pg_restore" --list "$ALOHA_DUMP"
```

Check each command succeeds. The listing verifies that the archive can be read;
it does not replace a restoration drill. With a system PostgreSQL installation,
use its installed client tools instead of these `Build/LocalPostgres` paths.
The destination is outside the repository; copy backups to another device/host
and encrypt them when stored or transferred. They contain private account data.

Also preserve `Examples/AlohaMassage/Content`, `Public`, `Config`, private content
backups and any required private media/outbox. Keep a matching copy of application
code and migrations. The database dump does not contain those files. For a coherent
whole-site backup, pause application/content writes while collecting that set;
PostgreSQL can remain running for `pg_dump`. Preserve production role/provisioning
and secret configuration separately: a single-database dump does not create
cluster-wide roles. [PostgreSQL documents that distinction here](https://www.postgresql.org/docs/15/backup-dump.html#BACKUP-DUMP-ALL).

To test restoration into a **new, unused local database**, in the same terminal:

```sh
"$ALOHA_PG_BIN/createdb" -h "$ALOHA_PG_SOCKET" -p 55432 -U aloha_dev -T template0 aloha_restore_test &&
"$ALOHA_PG_BIN/pg_restore" -h "$ALOHA_PG_SOCKET" -p 55432 -U aloha_dev -d aloha_restore_test --no-owner --no-privileges --single-transaction --exit-on-error "$ALOHA_DUMP" &&
"$ALOHA_PG_BIN/psql" -X -h "$ALOHA_PG_SOCKET" -p 55432 -U aloha_dev -d aloha_restore_test -v ON_ERROR_STOP=1 -c "SELECT version FROM accounts.schema_version; SELECT count(*) FROM accounts.users;"
```

Stop if the destination already exists; choose another unused name. This drill
leaves `aloha` untouched and makes the local role own restored objects; production
restoration needs its own roles/grants. Restore into an empty database **before**
running migrations. For a full application drill, use a separate configuration,
port, copied content and outbox; leave delivery disabled until restored jobs have
been reviewed. See [pg_restore](https://www.postgresql.org/docs/15/app-pgrestore.html).

For production, start with automated nightly backups, an off-machine copy and a
periodic restore drill. One possible retention policy is seven daily, four weekly
and twelve monthly copies. Daily backups can lose up to a day's changes; use more
frequent backups or WAL archiving/point-in-time recovery if that is unacceptable.
Automation and retention are recommendations here, not implemented features.

### Fresh start for this test installation

Resetting PostgreSQL removes accounts (including admins), sessions, consent,
email challenges, campaigns, queued jobs and reservation records.
**It does not reset the service menu or page content**:
those are saved in `Content/site.json` and `Public`.

For a reversible local reset:

1. Back up anything wanted. Stop the accounts launcher with Ctrl+C and confirm
   **both** its C++ backend and private PostgreSQL have exited. The launcher only
   stops processes it started; a database started separately may still be running.
   With the variables above, `"$ALOHA_PG_BIN/pg_ctl" -D "$PWD/Examples/AlohaMassage/Runtime/Accounts/pgdata" status`
   checks that cluster.
   Do not remove a PID file to force a reset.
2. Move the entire `Examples/AlohaMassage/Runtime/Accounts` directory to a private
   archive location outside the repository. This resets the **whole dedicated demo
   cluster**, including any restore-test databases; never use this procedure on
   a shared production cluster or delete individual files inside `pgdata`.
3. Archive `Examples/AlohaMassage/Runtime/Email` as well, so old test messages and
   reused job filenames cannot be confused with the new installation. Keep
   `Runtime/ContentBackups` if retaining the edited content.
4. Run `python3 Examples/AlohaMassage/accounts_dev.py start`, then bootstrap a new
   administrator. The launcher creates a fresh cluster/database and applies migrations.

For a content reset too, separately restore a chosen baseline `Content/site.json`
and assets with the backend stopped. A content-backup JSON is a wrapper: restore
its `document` member, not the entire wrapper. No reset has been performed by
writing these instructions. Production database resets should use explicit,
reviewed database administration procedures rather than deleting storage folders.
