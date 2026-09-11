# Admin, reservations and gift vouchers

Design roadmap. **Optional accounts are now implemented:** admin-created users,
login/logout, profiles, password changes, roles and access management. See
[the accounts guide](../../Docs/ACCOUNTS.md). The optional
[service editor](../../Docs/CONTENT_EDITOR.md) also implements menu forms, revision
checks, live publishing and private backups. Optional [native reservations](../../Docs/RESERVATIONS.md)
now add customer booking/history, staff guest appointments, resource availability,
day/week agendas and appointment state management. Other content editing,
payments, vouchers and reports below remain proposals.
[Customer email and newsletters](../../Docs/CUSTOMER_EMAIL.md) now implement
registration, verification, recovery, consent and queued admin campaigns using a
private local outbox. Resend delivery, signed delivery callbacks, failure visibility
and appointment reminders are implemented; live activation still needs domain/DNS/HTTPS setup. Google/Facebook login is not implemented yet. The public site
still reads `/api/v1/site`; its header now has a login dialog and role-aware account
navigation. The local booking demonstration runs alongside existing external links.

## Editing and statistics today

Admin login and profiles require the accounts-enabled configuration. There is no
default password or statistics page. Administrators can use the service-menu form
on `/admin` when the content editor is enabled.
Edit `Content/site.json` manually for other translated text, team, promotions,
courses and voucher information. Put images in `Public/assets` and reference
them using `/assets/...`. Stop the C++ process before manual JSON edits and restart
afterwards. Form-based publishing requires no restart. The Python
preview reloads JSON on each request.

The entire JSON endpoint is public: never put passwords, customer information,
appointments, unpublished drafts or payment settings in that file.

## Keep one reusable backend

Keep the same C++ executable and run a separate process per website, with its
own configuration, database credentials, database, runtime/upload directories
and secrets. These processes can share a machine; separate service users and
resource limits improve isolation. Separate machines provide a stronger boundary.

Add content, identity, booking and commerce capabilities incrementally to the
backend. Configuration enables the capabilities needed by each business; content
and templates provide the branding. Avoid an Aloha-specific server fork and
avoid loading several unrelated businesses into one process for this deployment.

Felis should contain only reusable C++ utilities. Roles, booking rules, discount
policies and payment integrations belong in this backend. Introduce shared
Felis abstractions when there is an actual second use, not in anticipation of one.

## First increment: real content administration

The `/admin` interface, accounts APIs and `/api/v1/admin/content/services` now
exist. The table below describes the broader roadmap; drafts, preview, rollback,
media and statistics endpoints remain future work:

| Area | Proposed responsibility |
| --- | --- |
| `/api/v1/auth/*` | Login, logout, session, password recovery |
| `/api/v1/admin/content/*` | Drafts, translations, preview and publishing |
| `/api/v1/admin/media/*` | Controlled image uploads and metadata |
| `/api/v1/admin/users/*` | Staff access management |
| `/api/v1/admin/statistics` | Aggregated operational reports |
| `/api/v1/me/*` | A customer's own profile and history |

Keep the public content contract usable by the existing frontend. A publish
operation produces a validated public snapshot; drafts and private tables never
enter that response. Use version checks to prevent two editors overwriting one
another and record who changed what. Provide preview and rollback to an earlier
published revision.

Start with three roles and explicit permissions:

- **Admin:** users, permissions, configuration, publishing, reports and refunds.
- **Operator:** appointments and assigned content-editing permissions; only the
  customer details needed for their work. No automatic access to user management
  or financial exports.
- **Customer:** their own profile, appointments, orders and eligible rewards.

Enforce permissions and ownership in every backend operation. Hiding buttons in
JavaScript is presentation only. Bootstrap the first administrator using a local
command with a password prompt, with no shared/default credentials. Store salted
password hashes using an established password-hashing implementation selected
when implementing this module; do not invent cryptography in Felis. Use opaque,
revocable server sessions with Secure/HttpOnly/SameSite cookies over HTTPS,
CSRF protection for mutations, login rate limits and expiring single-use reset
tokens. Staff MFA is a sensible follow-up. See the
[OWASP authentication guidance](https://cheatsheetseries.owasp.org/cheatsheets/Authentication_Cheat_Sheet.html).

For uploaded images, check actual decoded format, pixel dimensions and size;
re-encode supported raster images and generate storage names. Keep originals
private. Initially omit arbitrary HTML, scripts and SVG uploads. Store localized
alt text alongside approved public images.

## Our own reservation system

The first implementation is available in [RESERVATIONS.md](../../Docs/RESERVATIONS.md):
verified-account self-service booking, staff-entered guests and an operator agenda.
PostgreSQL is the source of truth, using a separate reservation schema in the site's
account database. A guest entered by staff does not need a customer account;
anonymous self-service booking and account claiming are possible later extensions.
The broader design below includes capabilities beyond this initial version.

Model services and duration variants, staff qualifications, staff shifts/breaks,
rooms/resources, closures and appointments. A couples massage needs two suitable
therapists and sufficient room capacity; a four-hands massage needs two therapists
for the same customer. Reserve all required resources in one transaction.

Availability is the intersection of business hours, staff schedules, suitable
resources and unoccupied time, including preparation/cleanup buffers. It must
also respect booking lead time, future booking horizon and cancellation policy.
Keep operating timezone `Europe/Sofia` and store appointment instants in UTC.
Handle ambiguous/nonexistent local times when daylight-saving time changes.

The browser's availability display is advisory. On submission, recompute and
reserve atomically in the database; return a conflict if another customer just
took the slot. PostgreSQL range exclusion constraints can enforce non-overlap
per resource, rather than relying on a read-then-insert check in C++ alone.
See [PostgreSQL range constraints](https://www.postgresql.org/docs/current/rangetypes.html#RANGETYPES-CONSTRAINT).
Use an occupancy row per exclusive resource; model capacity explicitly instead
of pretending a room shared by two customers is two independent rooms.

Use idempotency keys for booking submissions. If payment is required, give a
temporary hold a short configurable expiry and release it reliably; a browser
session timeout is not a booking hold. For bookings without payment, confirm
directly. Only an explicit staff action marks an appointment completed; elapsed
time alone must not grant loyalty credit. Track cancellation and no-show states.

Send confirmations/reminders through durable jobs. Store the appointment and its
notification job in the same database transaction, then let a worker deliver and
retry. A mail outage should not lose the reservation.

### Calendar providers

Provide a built-in calendar first, then a narrow provider adapter for Google or
another service. Sync confirmed appointments asynchronously, retain external
event IDs and process retries idempotently. Keep OAuth refresh tokens private
and encrypted with separately managed keys.

Begin with one-way export. Two-way sync introduces conflicts: a remote edit or
deletion must not silently cancel a paid booking. Define reconciliation rules
and flag unresolved conflicts for an operator. If external busy events influence
availability, stale sync must be visible and booking policy must define whether
to stop accepting new appointments until it recovers. Two independently writable
calendars cannot promise immediate conflict-free scheduling.

## Paid vouchers with printable PDFs and email

Yes. A buyer can select a service or amount, add the recipient's name and a short
message, pay, and receive a ready-to-print PDF automatically. Our proposed flow:

1. Create a pending order using the server's prices, currency and terms. Store
   the chosen language and an immutable snapshot of the purchased offer.
2. Redirect to a payment provider's hosted checkout. Card details go to the
   provider; our server stores its references and payment state.
3. Accept a signed payment webhook. Verify its signature against the raw request
   body, check the order, amount, currency and confirmed payment status. Deduplicate
   events and fulfil the order only once. A success-page redirect is not proof
   of payment; delayed payment methods must wait for actual success.
4. In one transaction, mark payment confirmed, issue one voucher and enqueue
   PDF/email work. A duplicate webhook or job retry must reuse that voucher.
5. A worker renders the PDF, saves it privately and emails it as an attachment
   with a private download option. Track delivery failures and allow an operator
   to resend the same voucher. Delivery retries must not issue another gift.
6. At the studio, an authenticated operator validates and redeems the code in an
   atomic operation. Record who redeemed it and for which appointment.

This follows the delivery/retry model described in the
[Stripe webhook documentation](https://docs.stripe.com/webhooks); Stripe is an
example provider, not a dependency or account selected for this project.

The PDF should include the studio identity, purchased service/duration or value,
recipient name, optional greeting, issue/expiry dates, booking instructions,
terms version and a unique code/QR. Use embedded fonts with Bulgarian glyphs.
Choose a print size with the family, such as A5, and avoid large ink-heavy
backgrounds. Do not print billing addresses or unnecessary contact details on
a gift. An HTML-to-PDF worker offers convenient template reuse; a C++ PDF library
could keep deployment smaller but needs more manual layout. Choose and approve
the renderer during implementation, after checking Cyrillic support and licensing.

Use cryptographically random voucher codes. The database is authoritative; a
printed PDF or QR alone does not establish validity. Do not expose PDFs under
`Public`, include customer details in QR URLs, or log redemption secrets. Download
links should expire and be revocable. Public checks, if offered, reveal minimal
information and are rate limited; redemption always requires staff authorization.

Decide before implementation whether vouchers are single-service or stored-value,
whether partial redemption is allowed, when expiry begins, and how extensions,
refunds and cancellations work. Refunds/chargebacks must suspend or invalidate
remaining value, with manual review if already redeemed. Keep payment, voucher,
delivery and redemption states separate; a failed email does not undo a payment.

## History, loyalty and useful statistics

Start with completed visits, future occupancy, cancellations/no-shows, popular
services, paid/refunded orders, voucher sales and outstanding voucher value.
Separate cash collected from service delivery and voucher redemption so reports
do not count voucher sales and redemption as two sales. Agree the accounting
definitions with the business before treating these as financial reports.

For “10% off the sixth massage”, count eligible **completed** appointments and
record each reward earned/used. Specify which services count, whether discounted
or voucher visits qualify, whether rewards repeat, and whether promotions stack.
Calculate discounts on the server and prevent concurrent double redemption.
Link historical guest visits only after verifying ownership, not merely by
trusting an email address supplied by a new account.

Traffic analytics is separate from these operational records. There is no tracking
cookie or analytics service in the example. Introduce only the measurements the
business needs; define retention, privacy/consent requirements and access rules
before adding marketing trackers. Keep customer history out of public JSON and
ordinary application logs. Back up the database and private media, and test restore.

## Suggested implementation order

1. Implemented: database migrations, staff authentication, editable/published
   content, audit, customer reservations/history and staff guest booking/agenda.
2. Activate the implemented Resend transport with a verified domain and HTTPS hosting; consider optional social login and
   anonymous self-service booking with verified account claiming.
3. Operational reports and explicit loyalty rules.
4. Hosted payment checkout, voucher ledger, PDF generation and email jobs.
5. Optional external calendar sync and more advanced reports.

Each increment should work independently. No Redis, message broker, separate
microservices or generic workflow engine is necessary initially: a database-backed
job table and a small worker are sufficient starting points. Before live replacement,
review copied prices and terms, implement redirects/SEO, configure HTTPS and
production security headers, and exercise booking conflicts, payment retries,
redemption races, backup restore and delivery failure recovery.
