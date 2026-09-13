# WebSite Backend

A C++ backend scaffold built on FelisFramework and Drogon. The initial target is
Linux, with one separately configured deployment per business.

Start with the [Aloha setup and operating guide](Examples/AlohaMassage/README.md#quick-setup-and-operation).
For the full local application, run `./Scripts/build.sh`, then `./Scripts/run.sh`
and open <http://127.0.0.1:8082/>. See [development commands](#development-commands)
for dependency installation, other run modes and tests.
Database sizing, manual backup/restore and safe test resets are covered in
[database operation](Docs/ACCOUNTS.md#disk-backups-and-a-fresh-test-database).

The backend has been built and smoke-tested on this Linux system with GCC 11.3
and the installed Drogon 1.9.12. The local compatibility build below avoids the
standard library's missing `<format>` implementation without installing packages.

## Current functionality

- Felis application initialization, execution and cleanup.
- Drogon HTTP server configured through JSON.
- `GET /health` returns HTTP 200 and `{"status":"ok"}`, with `Cache-Control: no-store`.
- Static HTML, CSS, JavaScript and image/font serving from a dedicated document root.
- Optional `GET /api/v1/site` endpoint serving a configured public JSON document.
- Optional [PostgreSQL/Argon2 accounts module](Docs/ACCOUNTS.md): login, profiles,
  admin account management, roles and revocable sessions. Disabled by default
  at build time and independently enabled per site at runtime.
- Optional [customer email and newsletters](Docs/CUSTOMER_EMAIL.md): verified
  self-registration, recovery, email changes, subscription consent and admin
  campaigns, Resend delivery, verified delivery callbacks and failure visibility.
  The example defaults to a private local outbox; no real email is sent.
- Shared [modular administration UI](Docs/ACCOUNTS.md#reusing-the-administration-interface)
  with per-site name/support/default language and independent feature sections.
- Optional [reservations](Docs/RESERVATIONS.md): resource availability, customer
  booking/history, staff guest bookings and day/week management. The public example
  includes a sign-in dialog and role-aware profile/management navigation.
- Optional [service-menu editor](Docs/CONTENT_EDITOR.md): admin forms for translated
  service content, prices and availability, with live publishing and private backups.
- A plain HTML/CSS/JavaScript [Aloha Massage example](Examples/AlohaMassage/README.md)
  with Bulgarian/English content and a frontend preview that needs no C++ build.
- Command-line argument and basic configuration validation.
- Drogon's default handling of SIGINT (Ctrl+C) and SIGTERM to stop its event loop.

The health endpoint indicates that the HTTP server can respond; it does not check
database readiness. Public-only sites require no database. Accounts-enabled sites
check their PostgreSQL schema at startup. Analytics, payments and
Google Calendar integration remain future work.

## Requirements

- CMake 3.25 or newer.
- A compiler and standard library supporting C++23, as required by FelisFramework.
- The populated FelisFramework submodule in `Dependencies/FelisFramework`.
- An installed Drogon development package exposing the CMake target
  `Drogon::Drogon`, including its transitive dependencies such as Trantor and JsonCpp.

The [apt installer](Scripts/install-deps.sh) handles system prerequisites. CMake
consumes the existing Felis source tree and locates an installed Drogon package;
it does not download either dependency. JsonCpp is supplied through Drogon, not
added as a separate dependency.
Felis examples and tests remain controlled by its existing CMake options, which
default to OFF; the build script enables Felis tests.

## Development commands

From the repository root:

```sh
./Scripts/install-deps.sh --dry-run
./Scripts/install-deps.sh
./Scripts/build.sh
./Scripts/run.sh
```

Run the installer when setting up a machine or adding missing system prerequisites.
It uses the configured Debian/Ubuntu apt sources, refreshes their package lists and
lets apt confirm installation. `--dry-run` simulates installation with the current
lists and makes no changes. The default includes PostgreSQL, libpq, Argon2 and
Node.js (used by integration tests); `--public-only` omits those extras.
Installing PostgreSQL packages may create/start a system database service. The
development launcher uses its own private cluster and Unix socket, independent of
that service. Build, run and test as your regular user.

The installer reuses an existing Drogon CMake package, or selects `libdrogon-dev`
when available from apt. If neither is available, it reports the remaining manual
step and links to [Drogon's installation guide](https://github.com/drogonframework/drogon/wiki/ENG-02-Installation).
For example, this machine's configured Debian 12 repositories have no Drogon
package; it already has Drogon installed under `/usr/local`. The installer does
not add package repositories or acquire the Felis submodule. On older distributions,
check that the installed CMake meets the 3.25 minimum.

| Command | Purpose |
| --- | --- |
| `./Scripts/build.sh` | Debug build in `Build`, with accounts, content editing, reservations and Felis tests enabled |
| `./Scripts/build.sh --release --jobs 4` | Release build in the same directory with four parallel jobs |
| `./Scripts/run.sh` | Full Aloha application on **8082**; starts private PostgreSQL and applies account/reservation migrations |
| `./Scripts/run.sh bootstrap your@email.com` | First administrator setup with a hidden password prompt |
| `./Scripts/run.sh reset-password your@email.com` | Local password recovery for an existing account |
| `./Scripts/build.sh --public-only` | Separate build in `Build/PublicOnly`, with optional modules disabled |
| `./Scripts/run.sh public` | Public-only C++ website on **8080**, without a database |
| `./Scripts/run.sh preview` | Frontend-only Python preview on **8090**; accepts `--port N` |
| `./Scripts/test.sh` | Build both variants, run Felis tests and the complete application integration suite |
| `./Scripts/test.sh --no-build` | Run those tests using existing builds |
| `./Scripts/clean.sh --dry-run` | Validate configured builds and preview their clean commands |
| `./Scripts/clean.sh` | Clean compiled outputs in both builds while preserving caches and runtime data |
| `./Scripts/clean.sh --public-only` | Clean only the public-only build |

`./Scripts/test.sh` is the single standard verification command: it builds the full
application, builds public-only, then runs the complete tests using those binaries.
Use `./Scripts/test.sh --jobs 4` to control build parallelism. A separate `check.sh`
would duplicate this workflow. After explicit builds (for example with custom
CMake options), use `./Scripts/test.sh --no-build`.

`clean.sh` delegates to CMake's clean target; it never recursively removes `Build`.
It skips unconfigured builds and rejects symlinked build directories/caches or
caches for another source/build path. Stop builds, tests and running backends first.
CMake caches/generators, `Build/LocalPostgres`, test fixtures and example `Runtime`
data are retained. Cache resets and database/backup removal remain deliberate
manual operations; cleaning does not give a fresh configuration or database.

All scripts support `--help`. Paths to project files resolve from the script, so
the scripts can also be invoked by absolute path from another directory. Server
commands stay in the foreground; Ctrl+C stops them. The full launcher preserves
existing data. Back up valuable data before applying new migrations.

Builds default to two jobs, or `CMAKE_BUILD_PARALLEL_LEVEL` when set. They reuse the
existing CMake cache/generator; fresh directories use Ninja. The scripts use
`FELIS_USE_STD_FORMAT=OFF` for the current compiler compatibility mode. Additional
CMake options go after `--`, such as `-DCMAKE_PREFIX_PATH=/opt/drogon` or
`-DFELIS_USE_STD_FORMAT=ON`. These options may override the script's defaults.
The supplied launch commands expect single-configuration Ninja/Makefile builds.

Integration tests use disposable database/content fixtures and a fake email
provider. They require local socket access and use HTTP ports 18082–18084 plus an
automatically assigned provider port; stop other test runs before starting another.
Runtime data, private email, backups, local environment files and generated caches/logs are excluded by
`.gitignore`. Ignore rules do not remove files already tracked in version control.

## Manual build

For a minimal public-only build, independent of the full development script:

```sh
cmake -S . -B Build/PublicOnly -DCMAKE_BUILD_TYPE=Debug -DWEBSITE_ENABLE_ACCOUNTS=OFF -DWEBSITE_ENABLE_CONTENT_EDITOR=OFF -DWEBSITE_ENABLE_RESERVATIONS=OFF
cmake --build Build/PublicOnly
```

For a Drogon installation outside the standard search paths, add
`-DCMAKE_PREFIX_PATH=/absolute/path/to/drogon/install` to the configuration command.
Use a compatible compiler, standard library and ABI for Drogon and the application.

### Local compatibility build (GCC 11 on this machine)

```sh
./Scripts/build.sh --public-only
ctest --test-dir Build/PublicOnly/Dependencies/FelisFramework --output-on-failure
./Scripts/run.sh public
```

Open <http://127.0.0.1:8080/>. This is the C++ server; the optional Python frontend
preview uses port 8090.

`FELIS_USE_STD_FORMAT=OFF` omits `Logger::LogFmt` and `LogFmtAt` and formats
timestamps using ordinary standard-library streams and chrono calendar types.
Stream-style logging remains available, including all logging used by this
backend. Timestamps retain UTC and six fractional digits. The option defaults to
ON, preserving the full API on a toolchain with `<format>` support. All consumers
receive the selected mode through `Felis::Framework`'s public compile definition.

The root project sets C++23 before finding Drogon to avoid this installed package's
filesystem checks restoring an empty `CMAKE_CXX_STANDARD`. Switching to libc++
was not needed; the backend retains the libstdc++ ABI used by the installed Drogon.

## Manual server invocation

From the repository root, using a single-configuration CMake generator:

```sh
./Build/WebSiteBackend
./Build/WebSiteBackend --help
./Build/WebSiteBackend --config=/absolute/path/to/server.json
```

Run only one server instance for a given listening address and port. The first and
third commands run in the foreground until stopped. Multi-configuration generators
may place the executable under an additional configuration directory.

The generic `Config/server.json` listens on `127.0.0.1:8080` over HTTP. The
development script instead selects the full Aloha configuration on port 8082. Adjust
`listeners` for a different address or port. HTTPS deployment is a later step.

The default configuration file is `Config/server.json`, resolved relative to the
process working directory. Pass an absolute `--config=PATH` when launching from
elsewhere. Options use the `--name=value` form supported by Felis; positional
arguments are rejected. `--help` does not read configuration or start the server.

## Configuration and static content

The file uses Drogon's JSON configuration structure. This application requires:

- An `app` object.
- A non-empty `listeners` array, with explicit addresses and ports from 1 to 65535.
- Non-empty `app.document_root` and `app.upload_path` strings.

Malformed JSON, duplicate keys, trailing data and the invalid required settings
above produce an initialization error. Remaining settings are interpreted by
Drogon. Configuration is loaded once at startup; changes require a restart.

The application resolves **`app.document_root`, `app.upload_path`, and the optional
`custom_config.public_content_file` relative to the configuration file's directory**.
The first two are converted to absolute paths before passing the configuration to
Drogon; the content file is loaded by this application. Absolute paths remain absolute. Use absolute
paths for any additional file-based settings added later, such as TLS certificates
or file logs; those retain Drogon's own path semantics.

With the supplied configuration:

- `../Public` refers to a `Public` directory at the repository root.
- `../Runtime/Uploads` refers to `Runtime/Uploads` at the repository root, outside
  the static document root.

The default root-level `Public` and runtime directories are not included.
Add your frontend to `Public` when ready, or configure another directory containing
public assets. `/` will serve its `index.html`; until that file exists, `/` returns
404 while `/health` remains available. Keep configuration, database files and other
private data outside the document root, including through symbolic links.

Drogon creates temporary request-body storage beneath the configured upload path
when run, so its parent location must permit the server to create/write that
storage. This does not provide an upload endpoint. Drogon's built-in sessions stay
disabled; the optional accounts module owns its PostgreSQL sessions. Static
response caching is disabled in this development configuration.

### Optional public content

Add this to a site's server configuration to enable `/api/v1/site`:

```json
"custom_config": {
  "public_content_file": "../Content/site.json"
}
```

The file must be a JSON object, at most 1 MiB, with a maximum parser stack depth
of 64. Missing/unreadable files, malformed JSON, duplicate keys and trailing data
fail initialization. `custom_config` must be an object if supplied, and an explicit
`public_content_file` must be a non-empty path. Omitting the setting leaves the
endpoint unregistered, preserving the original configuration's behavior.

Every field in this document is public. Keep it separate from server configuration,
credentials and customer records. Its contents are parsed and serialized once at
startup, then captured by the handler. `GET` and `HEAD` are supported, with JSON
content type, `Cache-Control: no-store` and `X-Content-Type-Options: nosniff`.
Manual content edits require stopping and restarting the C++ process. The optional
[service editor](Docs/CONTENT_EDITOR.md) adds authenticated publishing and replaces
the public snapshot after a save, without restarting.

The public reader does not impose the Aloha frontend's schema. Only the optional
service editor validates the reusable services/categories/locales contract.

### Run the Aloha example

```sh
./Build/WebSiteBackend --config=Examples/AlohaMassage/Config/server.json
```

This uses `Examples/AlohaMassage/Public` and `Examples/AlohaMassage/Content/site.json`.
For another business, supply a separate configuration, public directory, content
document and listening port. The same executable runs as a separate process for
each business. Give each deployment its own writable storage and, when persistence
is introduced, its own database credentials. See the example README for a local
frontend preview that does not compile or start the C++ server.

## Application structure

- `Source/Main.cpp`: entry point, exit status and last-resort exception reporting.
- `Source/ServerApplication.h` and `.cpp`: Felis lifecycle, configuration loading
  and explicit HTTP handler registration.
- `Source/PublicContent.h` and `.cpp`: bounded JSON loading and the optional public
  content endpoint; independent of any particular business's frontend.
- `Source/stdafx.h`: project precompiled header, following Felis conventions.
- `Config/server.json`: generic public-only listener and server settings.
- `Scripts`: apt prerequisites, development builds, example launch modes and tests.
- `CMakeLists.txt`: executable target and dependency integration.
- `Examples/AlohaMassage`: studio-specific content, configuration and frontend.

`OnInit()` validates arguments, loads configuration and registers handlers.
`OnRun()` calls Drogon's blocking `run()` on the main thread. Drogon's default
signal handlers request shutdown; after `run()` returns, Felis calls `OnDeinit()`
to flush the application logger. Expected initialization/runtime exceptions are
converted to Felis application errors and non-zero exit statuses. Lower-level
fatal failures inside dependencies may terminate the process directly.

Felis logs application lifecycle messages; Drogon retains its own framework
logging. Logger configuration is set before request processing begins. Felis's
main-thread-only timer manager is not used by request handlers.

## Extending the backend

Add business endpoint groups under `/api/v1/` as their features are implemented.
Keep HTTP parsing and response construction in controllers, business rules in
ordinary C++ service classes, and persistence in database access classes. Introduce
those files when the first real feature needs them.

Accounts provide authentication and role checks; the optional service editor uses
a generic protected-operation hook to publish content. The optional reservations
module runs authorization and resource allocation in a database transaction and
keeps schedules in configuration. Google Calendar synchronization and statistics
can build on its persisted appointment records in later increments.

## Verification

After building and starting the server on a working system:

```sh
curl -i http://127.0.0.1:8080/health
```

Verified on 8 September 2026 using the compatibility build:

- FelisTest: all 446 checks passed across ten component groups, including fixed
  UTC dates, leap day, fractional-second flooring and pre-epoch timestamps.
- Health and public content responses, UTF-8, response headers, HEAD and rejected POST.
- HTML, CSS, JavaScript and images served by Drogon; private source paths return 404.
- 120 content requests using twelve concurrent clients, with matching responses.
- Two separately configured processes serve independent content and document roots.
- Configuration-relative paths work when launched from a different working directory.
- Content stays unchanged until restart, then reloads the edited file.
- Invalid, duplicate-key, oversized, excessively nested and missing content fails startup.
- Omitting the content setting leaves `/api/v1/site` unregistered.
- Help, invalid command-line arguments and clean SIGINT/SIGTERM shutdown.

The small concurrency check establishes basic operation, not production capacity.
Browser interaction/visual checks and deployment hardening remain separate work.
Run `./Scripts/test.sh` to build both variants and execute the Felis and full
application suites. Accounts and service-editor verification are documented in
their respective guides. The underlying command is
`python3 Tests/accounts_integration.py --customer-email --email-delivery --reservations --content-editor`;
run it without those flags to check the optional features disabled.
Compatibility-mode tests exclude the unavailable formatted logging API while
retaining the ordinary logging, concurrency and timestamp checks.

## References

- [Drogon documentation](https://github.com/drogonframework/drogon/wiki)
- [Drogon configuration example](https://github.com/drogonframework/drogon/blob/master/config.example.json)
- [Drogon application API](https://github.com/drogonframework/drogon/blob/master/lib/inc/drogon/HttpAppFramework.h)
- [FelisFramework documentation](Dependencies/FelisFramework/README.md)
