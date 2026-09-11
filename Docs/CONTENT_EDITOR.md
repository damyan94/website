# Optional service-menu editor

Administrators can now add services and edit their translated names and
descriptions, category, booking link, availability, durations and display prices
using forms. This module is reusable by sites using the same service content
schema; it contains no Aloha-specific C++ code.

## Use the Aloha example

Build with both optional modules enabled, using this machine's existing dependency
paths from the CMake cache:

```sh
cmake -S . -B Build -DWEBSITE_ENABLE_ACCOUNTS=ON -DWEBSITE_ENABLE_CONTENT_EDITOR=ON -DFELIS_USE_STD_FORMAT=OFF
cmake --build Build --parallel 2
python3 Examples/AlohaMassage/accounts_dev.py start
```

Stop an older example process with Ctrl+C before starting the updated binary.
Open [Administration](http://127.0.0.1:8082/admin?lang=bg), sign in as an
administrator, and use **Меню с услуги / Service menu**. Use `lang=en` for English.
There is no default login; see [account setup](ACCOUNTS.md) to create the first
administrator with a hidden password prompt.

1. Choose an existing service or **Add service**.
2. Fill in the text for each site language. Select a category and booking link.
3. Add duration/price options as needed. Enter prices such as `55.00` or `55,00`;
   leave a price blank for “price on booking”. With no duration options, the
   public example offers a consultation. One currency applies to the service.
4. Choose whether booking is available. An unavailable service remains listed,
   with booking disabled.
5. **Save and publish**, then reload the public website on port 8082.

Publishing changes `Content/site.json` and the running server's public snapshot.
No restart is needed for changes made through the editor. The form warns before
discarding unsaved edits. If another administrator published first, the save is
rejected; copy your unsaved text before reloading the menu and applying it again.
After a timeout or storage error, reload to establish the current published state
before retrying. A response can fail after the file was replaced.
Publishing displays a progress state and allows up to 60 seconds for the response.
On this development machine, filesystem journal commits intermittently exceeded
the original 10-second UI timeout; a stalled worker was observed waiting in
`jbd2_log_wait_commit`. The longer timeout preserves the file/directory syncs.
Public HTTP reads continue on the last published snapshot while storage waits.

Edits do not change Fresha's services, availability or checkout prices. Those
destinations remain independently managed. The other site sections still use
manual content editing. For manual edits, **stop the server first**, edit the JSON
and restart. The editor is not a file watcher. Other running public-only processes
also retain their own startup snapshots until restarted.

## Optional configuration

Both CMake options default to `OFF`. Enabling the editor requires accounts support
and a POSIX host; this implementation was tested on Linux. No additional library
or database migration is needed beyond accounts. An accounts-only build can use
`WEBSITE_ENABLE_CONTENT_EDITOR=OFF`; its site must omit or disable `content_editor`.
The original public-only example and Python preview keep the editor disabled.

Alongside `accounts` and `public_content_file` in `custom_config`:

```json
"content_editor": {
  "enabled": true,
  "backups_directory": "../Runtime/ContentBackups",
  "currencies": ["EUR"]
}
```

Paths resolve relative to the server configuration. The content source and backup
directory must be outside the static document root; the backup directory cannot
contain the source. Final-component symlinks are rejected. Keep private paths out
of public roots through other aliases as well. The server user needs write access
to the source's directory because publishing replaces the file by rename. The
replacement file and backups are owner-only; the backup directory is owner-only.
Serve public content through the API, rather than granting a proxy access to the
source directory. Configuration changes require a restart.

The service schema has `schemaVersion: 1`, `locales`, `defaultLocale`, `categories`
and `services`. All configured languages must have a nonblank title/description.
Services have stable IDs, an existing category, an HTTPS or `tel:` booking URL,
and up to 20 unique whole-minute duration options. Prices are nonnegative integer
minor units or `null`; this editor uses currencies with two decimal places.
Allowed three-letter currency codes are supplied by the site owner. Titles are
limited to 200 Unicode code points, descriptions to 4,000, and durations to
1–1,440 minutes. Maximum price is 100,000,000 minor units. The whole document stays
within the existing 1 MiB limit, with at most 1,000 services. New IDs are assigned
by the server. Fields outside the edited service, and unknown fields already
present on it, are preserved.

## API and module boundaries

| Method and route | Purpose |
| --- | --- |
| `GET /api/v1/features` | Public flags `accounts` and `contentEditor` |
| `GET /api/v1/admin/content/services` | Admin: editor data and current revision |
| `PATCH /api/v1/admin/content/services` | Admin: publish one existing service |
| `POST /api/v1/admin/content/services` | Admin: publish a new service |

Writes accept exactly `{ "revision": "…", "service": { … } }`. A service contains
exactly `id`, `title`, `description`, `category`, `variants`, `available` and
`bookingUrl`. Use an empty ID for POST. Responses contain the refreshed editor
data, `serviceId`, and a `backupId` when the content changed. The revision covers
the complete public document, so simultaneous changes to different services also
conflict instead of overwriting one another. Unchanged saves create no backup.

The accounts module provides a generic protected-operation hook. It checks the
live session, role, mutation origin, request header and CSRF token, then invokes
the content module on its bounded worker queue. Operators and customers cannot
publish in this increment. Content writes are limited to 256 KiB per request and
do not run file I/O on Drogon's HTTP event loop. Public requests read complete
immutable snapshots. The frontend's `content.js` module is loaded only for an
enabled editor on the administrator page; there is no frontend build dependency.

## Persistence and backups

Each changed save validates the document, checks its revision, writes a private
backup, then writes and synchronizes a temporary source file before renaming it
over the previous source. Temporary files share the destination directory.
The public snapshot is replaced immediately after a successful source rename.
The parent directory is also synchronized, following the
[Linux fsync requirements](https://man7.org/linux/man-pages/man2/fsync.2.html).
This assumes a local filesystem that provides the expected rename/fsync behavior;
network filesystems and crash recovery were not tested.

An exclusive advisory lock in `<source>.editor.lock` prevents a second editor
process from owning the same content source. It is released when the process
exits; the empty lock file can remain. Each deployed website needs its own content
file and backup directory. Manual tools do not necessarily honor advisory locks,
so concurrent manual writes are unsupported. A completed manual edit changes the
revision, but that check cannot eliminate a race with an uncooperative writer.

Each backup JSON contains `document` (the previous full public document),
`previousRevision`, `replacementRevision`, `actorId` and `createdAtUnixMs`.
These record publication **attempts**: an interrupted save can leave a backup
without a published replacement. They are not a tamper-proof audit trail. Account
authorization commits before file I/O; PostgreSQL and filesystem changes are not
one transaction. Revocation prevents subsequent authorizations; an operation
already authorized may finish.

Backups are never automatically removed. At 512 backup JSON files the editor
refuses further changed saves until the machine owner archives older copies.
Keep that archive private and back it up independently. For manual recovery,
stop the server, preserve the current source, extract a chosen backup's `document`
into the source file, validate it, then restart. Copying the entire backup wrapper
over the source will not work. A rollback UI and automatic retention are deferred.

## Verification and current limits

```sh
python3 Tests/accounts_integration.py --content-editor
```

The integration suite uses the real C++ server and disposable PostgreSQL/content
fixtures. It exercises access restrictions, CSRF/origin checks, schema validation,
live publication, stale/concurrent revisions, backup failures, private paths,
restart persistence and disabled features. It never changes the actual example
content or user database. JavaScript syntax and decimal-price conversion are also
checked during implementation. Browser interaction and visual review are pending.

There are no drafts, scheduled publishing, rich-text editing, uploads, category
editing, service deletion/reordering, rollback UI or statistics in this increment.
Text is rendered as text, not executable HTML. General pages, promotions, team
and gallery editing can use separate focused forms later. Reservations remain
the next independent business capability.
