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

You can develop and run every automated test before buying anything. The local
outbox is the default. Production activation is a separate step:

1. **Choose a domain and hosting.** A domain is the website name you register,
   for example `your-domain.example` (a placeholder, not a real domain to use).
   Hosting is the computer/service that runs this C++ backend and PostgreSQL and
   keeps them online. It must support the backend, not just static HTML files.
   The company managing your domain's DNS may be different from your hosting company.
2. **Create a Resend account and add a sending domain.** A subdomain such as
   `mail.your-domain.example` keeps sending configuration distinct from the website.
   The outgoing address could then be `no-reply@mail.your-domain.example`.
   This does not create a mailbox for incoming replies; ordinary support email,
   if wanted, needs its own mailbox service.
3. **Copy Resend's DNS records into your DNS provider.** DNS is the public directory
   for your domain. Resend supplies exact record types, names and values for your
   selected domain/region. Its SPF record authorizes sending, and DKIM lets receiving
   servers verify signatures. Copy the displayed records exactly and wait for
   Resend to report the domain as verified. Follow its current DMARC guidance too.
   Keep existing website and mailbox records; do not replace unrelated MX records
   or invent a second SPF record for the same name.
4. **Deploy the website with HTTPS.** The hosting/reverse proxy supplies the TLS
   certificate and routes requests to this backend. Set `accounts.public_origin`
   to the final website origin, such as `https://www.your-domain.example`. All account
   links point there. The proxy must preserve that host and route `/api/v1/*` to the
   backend. The website can initially run with `local_outbox` while you finish setup.
5. **Register the webhook in Resend.** Use the website origin followed by
   `/api/v1/email/webhook/resend`, for example
   `https://www.your-domain.example/api/v1/email/webhook/resend`.
   This is the backend's built-in receiver, not a separate service you must write.
   Subscribe to `email.sent`, `email.delivered`, `email.delivery_delayed`,
   `email.bounced`, `email.complained`, `email.failed` and `email.suppressed`.
   Resend's webhook detail page supplies a signing secret beginning with `whsec_`.
6. **Set two secrets in the server's private environment.** Create a Resend API key
   with sending permission, restricted to the sending domain where available.
   Put it in `RESEND_API_KEY`; put the webhook signing secret in
   `RESEND_WEBHOOK_SECRET`. Use your hosting secret settings or the service manager's
   private environment. Neither belongs in JSON, public JavaScript, version control,
   chat messages or screenshots. These are separate secrets: one authorizes outgoing
   requests, the other verifies incoming delivery reports.
7. **Apply migrations, configure Resend, then restart.** Stop the previous backend,
   back up the database, apply accounts and reservation migrations using the owner,
   and apply the runtime grants below. Configure the following `customer_email`
   object under `custom_config.accounts`, adapting the sender and footer:

```json
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
```

8. **Test one address you control.** Request registration or verification through
   the website, follow the email link, then check **Email delivery** and Resend's
   dashboard. Expect `accepted` followed by `delivered`; an accepted message alone
   is not evidence of inbox arrival. If it stays accepted, inspect Resend's webhook
   delivery attempts and the endpoint/proxy configuration. A browser GET at the
   webhook URL returns 405; the endpoint accepts signed POST requests only.

`127.0.0.1` means the machine making the request. Resend cannot reach your local
computer using that address. Normal production delivery therefore requires an HTTPS
public origin. A public website and a verified sending domain are both needed for
working registration links; neither is needed for the automated local tests.

Resend provides the authoritative [domain verification instructions](https://resend.com/docs/dashboard/domains/introduction),
[webhook setup](https://resend.com/docs/webhooks/introduction), and
[webhook signing details](https://resend.com/docs/webhooks/verify-webhooks-requests).
The implementation follows [Svix's signed payload format](https://docs.svix.com/receiving/verifying-payloads/how-manual),
using the existing OpenSSL dependency. Provider quotas depend on your account;
check [Resend usage limits](https://resend.com/docs/api-reference/rate-limit) before
queueing a campaign.

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
