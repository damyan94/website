#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: Scripts/run.sh [full|public|preview|bootstrap EMAIL|reset-password EMAIL]

full                 Full Aloha application and private PostgreSQL on port 8082
                     (the default); applies migrations and preserves existing data.
public               Public-only C++ application on port 8080, without PostgreSQL.
preview [--port N]   Python frontend preview on port 8090, without a C++ build.
bootstrap EMAIL      Create the first administrator using a hidden password prompt.
reset-password EMAIL Reset an existing account password using a hidden prompt.

Build first with Scripts/build.sh (or Scripts/build.sh --public-only for public).
Keep the server in the foreground; Ctrl+C stops it. Use only one full launcher.
EOF
}

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:-full}
if (( $# )); then shift; fi
case "$mode" in
    -h|--help) usage; exit 0 ;;
    full|public)
        if (( $# )); then printf '%s does not accept extra arguments.\n' "$mode" >&2; exit 2; fi ;;
    bootstrap|reset-password)
        if (( $# != 1 )) || [[ -z "$1" || "$1" == -* ]]; then
            printf '%s requires one email address.\n' "$mode" >&2
            exit 2
        fi ;;
    preview) ;;
    *) printf 'Unknown mode: %s\n' "$mode" >&2; usage >&2; exit 2 ;;
esac

cd -- "$repo_root"
export PYTHONDONTWRITEBYTECODE=1
if [[ "$mode" == public ]]; then
    if [[ ! -x Build/PublicOnly/WebSiteBackend ]]; then
        printf 'Public-only executable is missing. Run Scripts/build.sh --public-only first.\n' >&2
        exit 1
    fi
    printf 'Public-only example: http://127.0.0.1:8080/ (Ctrl+C to stop)\n'
    exec "$repo_root/Build/PublicOnly/WebSiteBackend" \
        "--config=$repo_root/Examples/AlohaMassage/Config/server.json"
fi

if ! command -v python3 >/dev/null; then
    printf 'Python 3 is missing. Run Scripts/install-deps.sh first.\n' >&2
    exit 1
fi
if [[ "$mode" == preview ]]; then
    exec python3 "$repo_root/Examples/AlohaMassage/preview.py" "$@"
fi
if (( EUID == 0 )); then
    printf 'Run the local PostgreSQL launcher as your regular user, not root.\n' >&2
    exit 1
fi
if [[ ! -x Build/WebSiteBackend ]]; then
    printf 'Full executable is missing. Run Scripts/build.sh first.\n' >&2
    exit 1
fi
for feature in ACCOUNTS CONTENT_EDITOR RESERVATIONS; do
    if ! grep -Eq "^WEBSITE_ENABLE_${feature}:BOOL=(ON|TRUE|YES|Y|1)$" Build/CMakeCache.txt; then
        printf 'The full example needs all three modules. Run Scripts/build.sh first.\n' >&2
        exit 1
    fi
done
if [[ "$mode" == full ]]; then
    printf 'Full example: http://127.0.0.1:8082/ (Ctrl+C to stop)\n'
    mode=start
fi
exec python3 "$repo_root/Examples/AlohaMassage/accounts_dev.py" "$mode" "$@"
