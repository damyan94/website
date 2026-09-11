#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: Scripts/test.sh [--no-build] [--jobs N]

Build the full and public-only variants, run Felis tests, then run all application
integration checks (accounts, content editor, customer email/delivery, reservations).
Tests use a disposable PostgreSQL cluster and a fake email provider on loopback.
--no-build reuses binaries previously built with Scripts/build.sh.
--jobs controls build parallelism (CMAKE_BUILD_PARALLEL_LEVEL, or two by default).
Run as your regular user; PostgreSQL refuses to initialize as root.
EOF
}

build=true
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-2}
while (( $# )); do
    case "$1" in
        --no-build) build=false; shift ;;
        --jobs)
            if (( $# < 2 )); then printf '%s\n' '--jobs requires a positive integer.' >&2; exit 2; fi
            jobs=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    printf '%s\n' '--jobs / CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer.' >&2
    exit 2
fi
if (( EUID == 0 )); then
    printf 'Run the PostgreSQL integration tests as your regular user, not root.\n' >&2
    exit 1
fi
for tool in cmake ctest python3 node; do
    if ! command -v "$tool" >/dev/null; then
        printf '%s is missing. Run Scripts/install-deps.sh first.\n' "$tool" >&2
        exit 1
    fi
done

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd -- "$repo_root"
export PYTHONDONTWRITEBYTECODE=1
if "$build"; then
    "$repo_root/Scripts/build.sh" --jobs "$jobs"
    "$repo_root/Scripts/build.sh" --public-only --jobs "$jobs"
fi
if [[ ! -x Build/WebSiteBackend || ! -x Build/PublicOnly/WebSiteBackend ||
    ! -f Build/Dependencies/FelisFramework/CTestTestfile.cmake ]]; then
    printf 'Required binaries/Felis tests are missing. Run Scripts/test.sh without --no-build.\n' >&2
    exit 1
fi
ctest --test-dir "$repo_root/Build/Dependencies/FelisFramework" --output-on-failure --no-tests=error
exec python3 "$repo_root/Tests/accounts_integration.py" \
    --binary "$repo_root/Build/WebSiteBackend" \
    --public-binary "$repo_root/Build/PublicOnly/WebSiteBackend" \
    --customer-email --email-delivery --reservations --content-editor
