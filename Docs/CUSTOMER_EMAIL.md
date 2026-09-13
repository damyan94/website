# Customer accounts, email and newsletters

The optional accounts module now supports customer registration, email verification,
password recovery, verified email changes and subscriber-only newsletters. The
Bulgarian/English forms work with the reusable backend and PostgreSQL. No new
library or Felis change is required.

**Delivery supports both a private local outbox and the Resend HTTPS API.**
The example still uses the local outbox and sends no real email until you explicitly
configure Resend. Provider acceptance, actual mail-server delivery, failures,
bounces and complaints are tracked separately.
Optional [reservations](RESERVATIONS.md) now reuse the queue for booking notices.
Google/Facebook login, payments and voucher attachments remain later increments.

## Try the example

From the repository root, build the accounts-enabled binary as described in
[ACCOUNTS.md](ACCOUNTS.md), then run:

```sh
python3 Examples/AlohaMassage/accounts_dev.py start
```

The launcher applies migrations before starting HTTP. When updating an already
running demo, stop its launcher with Ctrl+C first, then start it again. The
existing database and administrator are preserved. Do not run the old binary
against the upgraded schema.

- [Register](http://127.0.0.1:8082/email?action=register&lang=bg): submit an email.
  Open the newest matching JSON file under `Examples/AlohaMassage/Runtime/Email`
  **on the local filesystem**, copy the link from its `text` field into your
  browser, and choose your name/password. A customer account is created only
  after this verification. Sign in through `/profile` afterwards.
- [Profile](http://127.0.0.1:8082/profile?lang=bg): edit contact information, verify
  an existing address, request a new email address, or subscribe/unsubscribe.
  Verification and subscription requests produce another private outbox file.
  Existing accounts keep working but are initially marked **unverified**; they
  must verify before email recovery, email changes or newsletter subscription.
- [Password recovery](http://127.0.0.1:8082/email?action=reset&lang=bg): request a
  reset for a verified account. Follow its outbox link and choose a new password.
  Resetting signs out all devices. Local owner recovery remains available for
  unverified accounts through the existing CLI.
- [Administration](http://127.0.0.1:8082/admin?lang=bg): sign in as an administrator,
  open **Newsletters and offers**, select a language, write a subject/plain-text
  message, preview the eligible recipient count and message, then confirm queueing.
  Refresh delivery status to see outbox writes, provider acceptance, mail-server
  deliveries, failures and skipped recipients. **Email delivery / Изпращане на
  имейли** shows individual jobs, errors, attempts, filters, pagination and worker
  health. This administrator panel also works when newsletters are disabled. Operators/customers cannot access campaign APIs.

Use `lang=en` for English. There are no default credentials; the bootstrap command
in the accounts guide creates the first administrator. Campaigns with zero eligible
subscribers cannot be queued. Signing up does not subscribe someone automatically:
registration has an unchecked consent checkbox. Subscribing later through the
profile requires a separate email confirmation.

The local outbox is deliberately not served over HTTP. Each JSON file has one
recipient, sender, subject, plain text and optional unsubscribe headers. Opening a
file does not send anything. For jobs whose `transport` is `local_outbox`,
`delivered` in the database means a durable outbox write. The interface labels
these separately from delivery through Resend.

## Configuration and modularity

Inside `custom_config.accounts`:

```json
"customer_email": {
  "enabled": true,
  "registration_enabled": true,
  "newsletters_enabled": true,
  "transport": "local_outbox",
  "sender": "no-reply@example.test",
  "outbox_directory": "../Runtime/Email",
  "footer": "Aloha Massage · +359 879 925 093 · Newsletter / Бюлетин"
}
```

Paths resolve relative to the server configuration. The outbox must be outside
`app.document_root`; it must not be a symlink. Keep its parents private and do not
expose it through proxy aliases. The worker creates a mode-0700 directory and
mode-0600 message files and takes an exclusive local outbox lock. Use a distinct
outbox, process and database for each site. The current email worker requires POSIX.
A database advisory lock elects one active delivery worker; other backend instances
wait to take over if it exits. The local filesystem lock still rejects two instances
using the same outbox directory. Instances sharing a database should use matching
email/reservation policies and, for local transport, separate outbox directories.

Omitting `customer_email` or setting `enabled: false` disables its routes, forms
and worker. Login, administrator-managed accounts and content editing continue to
work. Registration and newsletters can also be disabled independently while
verification/recovery remain available. Pending registration/subscription links
cannot complete while their feature is disabled; a running email worker skips
queued messages for disabled features. Disabling the entire email module pauses
its queue until it is enabled again. Configuration changes require a restart.

`GET /api/v1/auth/options` reports enabled email features and supported languages.
It also includes the public `ui` presentation settings documented in
[admin reuse](ACCOUNTS.md#reusing-the-administration-interface).
The existing `/api/v1/features` contract remains unchanged. Compiling accounts out
with `WEBSITE_ENABLE_ACCOUNTS=OFF` omits all this code and its database requirements.
The content editor remains independently configurable.

## Going live, starting without a domain or hosting

The current Resend path needs no code change for activation. A verified domain,
private secrets, HTTPS hosting and live verification are still required. The
example configuration deliberately remains on `local_outbox`.

### Domain and Resend account

1. Choose a public website origin, for example `https://www.your-domain.example`,
   and a sending subdomain you own, for example `mail.your-domain.example`.
   Replace these placeholders. Hosting must run the C++ backend on POSIX and provide
   PostgreSQL; static hosting alone is insufficient.
2. Add the sending subdomain in Resend and choose its sending region. Copy the
   exact DKIM and SPF records Resend supplies: TXT and MX records, or CNAME records
   where shown. Keep verification CNAMEs DNS-only. Wait for **Verified**, then
   configure DMARC using Resend's guidance. Preserve existing website/mailbox
   records; do not replace unrelated MX records or add a second SPF policy at the
   same name. Values depend on your domain and region, so there is no universal
   DNS snippet to copy. See [Resend domain setup](https://resend.com/docs/add-a-domain).
3. Create an API key with `sending_access`, restricted to that verified domain.
   Use a separate key for staging. Check the team's current sending quota and rate
   limit before activation. See [API key permissions](https://resend.com/docs/api-reference/api-keys/create-api-key)
   and [usage limits](https://resend.com/docs/api-reference/rate-limit).

Use a sender such as `no-reply@mail.your-domain.example` at the exact verified
sending domain. This application requires a bare ASCII address and lowercases it;
`Studio <no-reply@...>` is not an accepted setting. Sending-domain verification
does not create a reply mailbox. Keep provider click/open tracking disabled for
account links; this is a Resend domain setting, not an application setting.
See [Resend domain options](https://resend.com/docs/dashboard/domains/introduction).

### Private environment and application settings

Supply these variables to the **backend service process** through the hosting
secret store or a protected service-manager environment file. The backend reads
the process environment; it does not automatically load a `.env` file.

| Environment variable | Required value |
| --- | --- |
| `ALOHA_DATABASE_URL` | Existing PostgreSQL connection string for the site's runtime role; this name matches the example's `database_url_env`. |
| `RESEND_API_KEY` | The sending API key, copied unchanged. |
| `RESEND_WEBHOOK_SECRET` | The endpoint's complete signing secret, including `whsec_`; do not Base64-decode it yourself. |

Only variable **names** belong in JSON. Keep values out of public files, logs and
command arguments; disable environment/body/Authorization-header dumps in the
service/proxy. `GET /api/v1/auth/options` exposes selected presentation fields,
feature flags and transport only. Delivery status omits secrets/message bodies;
secret-loading errors identify the setting or variable name without its value.

Merge this partial object into `custom_config.accounts` in the private deployment
configuration. Retain `ui_root`, schema paths, session settings and unrelated
configuration. Keep your existing registration/newsletter policy; `true` below
matches the example.

```json
{
  "enabled": true,
  "database_url_env": "ALOHA_DATABASE_URL",
  "public_origin": "https://www.your-domain.example",
  "allow_insecure_loopback": false,
  "customer_email": {
    "enabled": true,
    "registration_enabled": true,
    "newsletters_enabled": true,
    "transport": "resend",
    "sender": "no-reply@mail.your-domain.example",
    "footer": "Your business name and contact details",
    "resend": {
      "api_key_env": "RESEND_API_KEY",
      "webhook_secret_env": "RESEND_WEBHOOK_SECRET",
      "request_seconds": 10
    }
  }
}
```

The origin has no path, trailing slash or explicit default `:443` port. The footer
is required (1–500 characters), even when newsletters are disabled.
`request_seconds` defaults to 10 and accepts integers 1–30. Omit `resend.test_port`:
it is a loopback fixture option and is rejected for HTTPS origins. Real requests
use `https://api.resend.com/emails` with certificate verification enabled.
`outbox_directory` is unused by Resend; there is no automatic fallback to local
delivery when a provider request fails.

### Webhook and HTTPS routing

Register this exact endpoint in the same Resend team used for sending:

```text
https://www.your-domain.example/api/v1/email/webhook/resend
```

Select `email.sent`, `email.delivered`, `email.delivery_delayed`, `email.bounced`,
`email.complained`, `email.failed` and `email.suppressed`. Copy that webhook's
signing secret into `RESEND_WEBHOOK_SECRET`; it is separate from the API key.
Other event types are acknowledged and ignored. See
[webhook setup](https://resend.com/docs/webhooks/introduction) and
[event types](https://resend.com/docs/webhooks/event-types).

Terminate TLS with a publicly trusted certificate at the host/reverse proxy. The
backend can retain its loopback HTTP listener (the example uses
`127.0.0.1:8082`), while `public_origin` remains the external HTTPS origin.
Forward the canonical `Host` header exactly, including any nondefault port:
rewriting it to the upstream address causes rejection. `X-Forwarded-Host` and
`X-Forwarded-Proto` do not replace this check or configure the origin.

Route the website, account pages and `/api/v1/*` to the correct backend. The webhook
must reach its POST handler without redirects, browser login, proxy Basic Auth or
a browser challenge. Preserve `svix-id`, `svix-timestamp`, `svix-signature` and
the exact request-body bytes; JSON rewriting breaks signatures.
[Resend signature requirements](https://resend.com/docs/webhooks/verify-webhooks-requests)
match the existing receiver. Synchronize the backend host clock: signatures outside
a five-minute timestamp tolerance are rejected. Allow outbound DNS and HTTPS to
`api.resend.com`; keep the upstream HTTP port private.

Keep deployment configuration, secret files, uploads and the local outbox outside
the served document root and proxy aliases. Do not log message bodies or capability
tokens; exclude the one-click unsubscribe query from access logs.

### Activate and verify one real message

1. **Prepare.** Use `Build/WebSiteBackend --config=PATH` with the private deployment
   file, Accounts schema version 4 and its [runtime grants](#schema-upgrade-and-runtime-grants).
   Switching transports requires no migration. Configuration paths are relative
   to that file. `Scripts/run.sh` / `accounts_dev.py` is the local demo launcher;
   the public-only binary has no mail worker.
2. **Review the queue before switching.** Use a separate staging database containing
   only addresses you control, or inspect the existing queue before live activation.
   Unattempted eligible jobs will use Resend after the switch; previously attempted
   local jobs remain pinned to their old transport and are skipped. Starting the
   worker is not a dry run. Stop the old service, supply the environment/configuration,
   restart, and enable the Resend webhook. Instances sharing a database must use
   matching settings/secrets.
3. **Confirm the active configuration.** Open `/api/v1/auth/options` and check
   `"email": true` and `"transport": "resend"`. Sign in as an administrator at
   `/admin?lang=en`, open **Email delivery**, and check the worker heartbeat,
   error and pause status. The panel uses authenticated
   `GET /api/v1/admin/mail`; operators/customers cannot view it. The webhook route
   exists only with enabled Resend delivery. A browser GET returns 405 and an
   unsigned POST returns 400; neither proves that a signed callback works.
4. **Request exactly one message to a mailbox you control.** For an existing
   unverified account, sign in at `/profile?lang=en` and request email verification.
   Alternatively, when registration is enabled, submit an unused controlled
   address at `/email?action=register&lang=en`. A generic accepted request alone
   does not prove a job was queued. Avoid repeated submissions and campaigns.
5. **Check delivery and the link.** Refresh Email delivery and identify the new job
   by recipient/time. Match its `providerId` to Resend. Expect `accepted`, then
   `delivered` (a fast callback can hide the intermediate state). Check actual inbox
   arrival and From address separately, inspecting spam if absent. Complete the
   verification link within 20 minutes. Mail-server acceptance does not establish
   inbox placement.
6. **Confirm callback reconciliation.** In Resend's webhook attempts, locate
   `email.delivered` for that provider ID and confirm HTTP 200 from this endpoint.
   Refresh the panel: its matching `delivered` state is read from PostgreSQL.
   Optionally inspect event metadata with the query below. Replay the same successful
   event from Resend: expect 200, unchanged job state/attempts and no extra message.
   The same `svix-id` remains one stored event; replay does not resend the email.
   [Resend replay instructions](https://resend.com/docs/webhooks/introduction).
7. **Check failure/retry visibility separately.** The local checks below cover
   timeouts, 429/503, permanent rejection, suppression and lost acknowledgments.
   For an optional hosting-level check, use an isolated staging service/database:
   block only its outbound Resend connection, request one verification message to
   a controlled address, and observe `queued`, increased `attempts`, a connection/
   timeout error and later `availableAt`. Restore egress before expiry/five attempts;
   wait for automatic delivery of the same job and confirm its inbox/callback result.
   This sends one additional real message. Do not edit jobs/keys, corrupt live
   credentials or generate customer bounces/complaints.

For optional database confirmation, replace `123` with the job ID recorded above
and run this read-only query using your existing private database access. It omits
message bodies, recipients, secret values and retry keys:

```sql
SELECT j.id, j.transport, j.state, j.attempts, j.provider_id,
       e.event_id, e.event_type, e.occurred_at
FROM accounts.mail_jobs AS j
LEFT JOIN accounts.mail_events AS e ON e.provider_id = j.provider_id
WHERE j.id = 123
ORDER BY e.occurred_at, e.event_id;
```

If delivery stays `accepted`, inspect webhook attempts first: 400 can mean the
wrong Host/secret, altered body, clock skew or invalid event data; 404/405 suggests
route/method/configuration trouble; 503 requires checking backend/database
availability. A provider 401/403 becomes `failed` with `provider_configuration`
and a five-minute provider pause. Transient failures expose their category,
attempt count and next availability; 429/5xx also pause provider requests.
Permanent/exhausted failures remain visible under the **failed** filter. After
resolving them, request a fresh account link; there is no manual retry button.
Do not clear local suppressions merely to make a test pass. See
[operational behavior](#delivery-status-and-operational-behavior) and
[queue limits](#durable-queue-and-limits).

Recommended local verification, when execution is approved:

```sh
bash Scripts/build.sh
bash Scripts/build.sh --public-only
bash Scripts/test.sh --no-build
```

These use an isolated database and fake provider; no real mail is sent. Record
live job/provider/event IDs, webhook HTTP result, inbox result and any separate
retry result as deployment evidence, excluding links/secrets. This guide does not
claim those deployment steps have been run.

## Delivery status and operational behavior

The admin panel shows the latest 50 jobs per page, with a state filter and older-page
navigation. It never returns message bodies, verification links, raw unsubscribe
tokens, request snapshots or credentials. It shows safe error categories rather than
unfiltered provider responses. A worker heartbeat older than 60 seconds is flagged;
provider pauses and reminder scheduler errors are visible separately. Refresh the
panel to retrieve the latest status.

- `queued` / `processing`: waiting, sending, or recovering an interrupted attempt.
- `accepted`: Resend returned a message ID; delivery is still pending.
- `delivered`: the recipient's mail server accepted the message, according to a
  verified provider event. This does not prove inbox placement or reading.
- `delayed`: the provider is still attempting delivery.
- `failed` / `bounced` / `complained`: sending failed, mail was permanently rejected,
  or the recipient reported spam.
- `skipped`: expired, suppressed or no longer eligible, or its transport changed
  after an earlier attempt. Local outbox writes have their own display label.

Webhook signatures cover the unmodified body, event ID and timestamp. Requests
outside a five-minute timestamp tolerance are rejected. Event IDs deduplicate
callbacks, and out-of-order events cannot turn a bounce/complaint back into success.
Events arriving before the outbound acknowledgment are retained and reconciled
when its provider message ID is recorded. Unknown event types are acknowledged and
ignored; the database stores only the supported event metadata, not the raw payload.

Bounces, complaints and provider suppressions block future sends to that address,
including account messages. They never silently change a user's email. There is no
automatic suppression release or manual retry button in this increment. After fixing
a sending configuration problem, request a fresh account link. Any deliberate release
of an address requires resolving the reason first; changing the provider dashboard
alone does not remove the local suppression record.

## Account and consent design

Accounts retain a permanent numeric ID. Email is a verified, changeable login
address; phone is optional contact information and is not a verified login ID.
Registration requests reserve no account and set no password before mailbox proof.
This prevents a stranger from pre-creating an account with their chosen password
at someone else's address.

Links use 256-bit random tokens, SHA-256 lookup digests, a 20-minute expiry,
single-use consumption and an explicit purpose. Password/access changes invalidate
pending account-bound links through a credential version. Email changes require
the current password, an already verified current address and confirmation of the
new address; completion revokes all sessions, removes the old subscription and
queues a notice to the old address. Recovery also queues a password-change notice.
Opening a link performs no mutation: the form requires confirmation. Reset and
email-change completion do not sign the user in automatically.

Registration and recovery request responses use the same generic wording for
eligible and ineligible addresses. Both perform Argon2 work, although this does
not promise indistinguishable end-to-end timings. Recipient limits persist in
PostgreSQL: at most one link per minute and five per hour across link purposes.
Existing bounded HTTP throttles also apply: 120 operations/minute globally,
60 per direct peer and 10 per identity/session. Behind a reverse proxy, add its
client-IP controls; the backend sees the proxy as the peer.

Email links put the capability token in the URL fragment, which HTTP requests do
not send; the page removes it from visible history and sets `Referrer-Policy:
no-referrer`. The RFC one-click endpoint necessarily uses a URL query capability;
exclude its query from proxy/access logs. Never log request bodies or tokens.

Only enabled accounts with a verified current email and confirmed consent are
eligible for newsletters. Audiences are filtered by the account's preferred
language. Consent records include timestamp, address, source/wording version and
an event history. Account/security messages do not depend on marketing consent.
Each promotion includes an unauthenticated unsubscribe capability. Profile
unsubscribe applies immediately; email links cannot access or change a password.
Old unsubscribe links cannot cancel a later, separately confirmed subscription.
No open/click trackers or subscription-by-admin override are implemented.

This follows [OWASP recovery guidance](https://cheatsheetseries.owasp.org/cheatsheets/Forgot_Password_Cheat_Sheet.html)
and the POST-based [one-click unsubscribe specification](https://www.rfc-editor.org/info/rfc8058/).
It is a starting implementation with integration tests, not an independent security audit.

## Durable queue and limits

Business changes and message jobs commit in the same PostgreSQL transaction. A
separate worker claims jobs using row locks, then commits before network/filesystem
I/O, so a slow provider or disk does not hold the account transaction open. Security messages
have priority over newsletters. Delivery processes at most five jobs/second, with five attempts, exponential
retry delays starting at 30 seconds, and a five-minute interrupted-job lease.
Resend request timeouts default to 10 seconds (configurable from 1 to 30). HTTPS
certificate validation is always enabled; production requests use the fixed Resend
API endpoint. HTTP 429/5xx failures back off across the whole worker, including
across restarts; numeric Retry-After values up to one day are honored. Credential/
permission failures stop that job and pause provider requests for five minutes.
Other permanent rejections stop that job immediately.

`Accounts::EmailDeliveryService` owns claim processing, eligibility coordination,
persisted payload/key snapshots, provider dispatch, retry decisions and delivery
reconciliation. `EmailWorker` owns the thread and loop, stop/join behavior, database
connection and recovery, scheduling cadence and heartbeat updates. The service
borrows settings, hooks and transports and receives the current connection for
each delivery attempt; it does not retain the connection across reconnects.

`ResendTransport` owns the HTTP client lifecycle, request submission and bounded
response decoding; `LocalOutboxTransport` owns the directory lock and atomic file
delivery. The worker starts the configured transport and joins its thread before
releasing transport resources. Neither transport accesses the database or retries
a delivery.

`MailRepository` borrows the caller's database connection for mail-job and provider-
event SQL, suppression records, worker status and delivery-status read projections.
It maps rows without owning transactions, reconnects or delivery decisions. The
delivery service commits a claim and its exact payload/key before transport I/O,
then reconciles the result in a separate transaction.

`EmailController` owns webhook host/signature checks, strict JSON and event-field
parsing, delivery-status filters and response construction. It reuses the Accounts
response handling for headers, cookies and errors. The status route keeps its
existing Accounts authentication and administrator check before filter validation;
its reads remain inside that authenticated transaction.

`EmailStatusService` borrows the Accounts worker's connection for status reads and
callback processing. It owns the callback transaction, including provider locking,
event insertion, replay/conflict checks and reconciliation through the unchanged
`ApplyEmailEvents` helper. Conflicting event IDs leave the transaction uncommitted
and produce the existing HTTP error in the controller. The controller owns no
transactions; `AccountStore` retains a forwarding entry point for worker access.
Account challenge and campaign workflows retain their own persistence for this stage.

Campaign preview stores a draft with an idempotency key bound to its author and
content. Queueing checks the displayed recipient count again; a changed count
requires another preview. It then snapshots the currently eligible recipients
atomically. Preview is a count, not a frozen mailing list. Repeating queueing for
an already queued campaign does not insert duplicates; the database also enforces
one job per campaign/account.

Delivery rechecks consent generation, verified address, account access, preferred
language and feature configuration. Unsubscribing cancels queued promotions. A
message already claimed for delivery may finish. Resend requests retain the exact payload and a random idempotency key through
retries/restarts, including when the provider accepted a request whose response was
lost. Automatic retries stop 23 hours after the first attempt, before Resend’s
[24-hour deduplication window](https://resend.com/docs/dashboard/emails/idempotency-keys)
expires. There is no unlimited exactly-once guarantee. A job that has been attempted
is pinned to its transport; changing transports skips it instead of resending it.
The local transport uses a stable filename per job and atomic replacement, so
retries do not create duplicate files.

Current bounds: 1,000 recipients per campaign, 10,000 pending jobs, subject length
200 characters and plain-text body length 5,000 characters. The admin lists the
50 most recent campaigns. There is no attachment/HTML editor, scheduled campaign
UI or manual retry UI yet. Accepted/completed/failed/skipped jobs clear message
bodies, outbound request snapshots and raw unsubscribe tokens; the outbox retains delivered
content, including any account links. The database retains campaign copy,
recipient metadata, consent history, delivery event metadata, suppressions and hashed
capabilities. No automatic retention cleanup is implemented. Define retention and disk monitoring before live use.
Private database/outbox backups contain personal information; queued email bodies
necessarily contain the raw verification link until delivery, despite hashed
capability lookup columns.

## Schema upgrade and runtime grants

`--migrate-accounts` upgrades version 1 to version 2 using
`Migrations/002_customer_email.sql`, then to version 3 using
`Migrations/003_user_activity.sql` for last-login tracking, then to version 4 using
`Migrations/004_email_delivery.sql` for delivery records, events, suppression and worker
health. Old completed jobs are labeled as local deliveries; previously attempted
local jobs remain pinned to that transport. Reservation schema version 2 adds durable
reminder records through `Migrations/Reservations/002_reminders.sql`. The CLI
serializes version decisions and is safe to repeat. Run it using the schema owner, with the old backend stopped;
normal startup never applies DDL. Back up the database before deployment. All
existing accounts, password hashes, roles and sessions are retained; no existing
email or marketing subscription is silently verified/approved.

Apply these additional grants to the per-site runtime role after migration:

```sql
GRANT SELECT, INSERT, UPDATE ON accounts.email_challenges,
  accounts.subscriptions, accounts.subscription_events,
  accounts.newsletter_links, accounts.campaigns, accounts.mail_jobs,
  accounts.mail_events, accounts.mail_suppressions, accounts.mail_worker_status TO aloha_app;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA accounts TO aloha_app;
```

These supplement the accounts guide's base grants. The new subscription tables
are also consulted by profile/access operations when email delivery is disabled.
The future identity table has no runtime API and needs no runtime grant yet.

`accounts.login_identities` prepares provider/issuer/subject-to-user associations.
It is not a social login implementation. Google/Facebook integration must verify
provider responses and link only after authentication/proof of ownership; matching
an untrusted email or phone alone must never merge accounts or booking history.

## Verification and next integration

```sh
python3 Tests/accounts_integration.py --customer-email --email-delivery --reservations --content-editor
python3 Tests/accounts_integration.py
```

Tests use an isolated PostgreSQL cluster, temporary content and a private outbox
under `Build`. They cover registration, expiry/purpose/replay, recovery and session
revocation, email changes, consent, language filtering, campaign idempotency,
unsubscribe, failed transport/restart, persistent recipient throttles, safe version-1
upgrades, disabled-feature delivery, private paths and the existing account/editor
boundaries. They send no real email. Browser interaction/visual testing is pending.

The optional `--email-delivery` checks use a loopback HTTP provider and generated
webhook signatures. They exercise provider acceptance, callbacks arriving before
acknowledgment, signature/replay checks, timeout recovery with identical payloads,
rate-limit backoff, permanent errors, suppression and access boundaries. Test-only
`resend.test_port` selects this local provider; it is rejected for HTTPS public
origins. It is not an option for bypassing TLS on a live provider.

[Reservation confirmations, changes and optional reminders](RESERVATIONS.md) use
the same queue. Paid PDF vouchers could reuse the pattern, but voucher delivery,
attachments, automatic retention cleanup and suppression management remain future
work. Live domain verification, DNS, HTTPS deployment and an actual inbox test are
operator setup steps, not results claimed by the local integration tests.
