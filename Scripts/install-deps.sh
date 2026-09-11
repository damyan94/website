#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: Scripts/install-deps.sh [--dry-run] [--public-only]

Install development prerequisites from the configured Debian/Ubuntu apt sources.
The default includes PostgreSQL, account libraries and Node.js for the tests.
--public-only omits those extras; Python frontend preview is still available.
--dry-run uses apt simulation and existing package lists; it changes nothing.

A normal run refreshes package lists, then lets apt ask before installing.
PostgreSQL packages may create/start a system service; the demo uses its own cluster.
Drogon is reused when found locally, otherwise installed through apt if available.
FelisFramework must already be populated in Dependencies/FelisFramework.
EOF
}

dry_run=false
public_only=false
for argument in "$@"; do
    case "$argument" in
        --dry-run) dry_run=true ;;
        --public-only) public_only=true ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$argument" >&2; usage >&2; exit 2 ;;
    esac
done

if ! command -v apt-get >/dev/null || ! command -v apt-cache >/dev/null; then
    printf 'This installer requires Debian/Ubuntu apt tools. See README.md for prerequisites.\n' >&2
    exit 1
fi

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
packages=(build-essential cmake ninja-build pkg-config ca-certificates
    libjsoncpp-dev libssl-dev zlib1g-dev uuid-dev libbrotli-dev python3)
if ! "$public_only"; then
    packages+=(libpq-dev libargon2-dev postgresql postgresql-client nodejs)
fi

privilege=()
if ! "$dry_run"; then
    if (( EUID != 0 )); then
        if ! command -v sudo >/dev/null; then
            printf 'Run this installer as root, or install sudo first.\n' >&2
            exit 1
        fi
        privilege=(sudo)
    fi
    "${privilege[@]}" apt-get update
fi

# Check CMake package files without configuring/building anything as root.
cached_drogon=''
if [[ -f "$repo_root/Build/CMakeCache.txt" ]]; then
    cached_drogon=$(sed -n 's/^Drogon_DIR:PATH=//p' "$repo_root/Build/CMakeCache.txt")
fi
shopt -s nullglob
drogon_configs=(
    "${Drogon_DIR:-}/DrogonConfig.cmake"
    "$cached_drogon/DrogonConfig.cmake"
    /usr/local/lib/cmake/Drogon/DrogonConfig.cmake
    /usr/local/lib64/cmake/Drogon/DrogonConfig.cmake
    /usr/local/lib/*/cmake/Drogon/DrogonConfig.cmake
    /usr/lib/cmake/Drogon/DrogonConfig.cmake
    /usr/lib/*/cmake/Drogon/DrogonConfig.cmake
    /usr/share/cmake/Drogon/DrogonConfig.cmake
)
IFS=: read -r -a prefixes <<< "${CMAKE_PREFIX_PATH:-}"
for prefix in "${prefixes[@]}"; do
    [[ -n "$prefix" ]] || continue
    drogon_configs+=("$prefix/lib/cmake/Drogon/DrogonConfig.cmake"
        "$prefix/lib64/cmake/Drogon/DrogonConfig.cmake"
        "$prefix"/lib/*/cmake/Drogon/DrogonConfig.cmake
        "$prefix/share/cmake/Drogon/DrogonConfig.cmake")
done

drogon_found=false
for config in "${drogon_configs[@]}"; do
    if [[ -f "$config" ]]; then
        printf 'Reusing Drogon CMake package: %s\n' "$config"
        drogon_found=true
        break
    fi
done
missing_drogon=false
if ! "$drogon_found"; then
    candidate=$(apt-cache policy libdrogon-dev | awk '/Candidate:/ {print $2}')
    if [[ -n "$candidate" && "$candidate" != '(none)' ]]; then
        packages+=(libdrogon-dev)
    else
        missing_drogon=true
        printf '%s\n' \
            'No installed Drogon package or apt candidate was found.' \
            'The other prerequisites can be installed, but Drogon must be installed separately:' \
            'https://github.com/drogonframework/drogon/wiki/ENG-02-Installation' >&2
    fi
fi

if "$dry_run"; then
    printf 'Dry run using current apt lists (a normal run refreshes them first).\n'
    apt-get --simulate install --no-install-recommends "${packages[@]}"
else
    "${privilege[@]}" apt-get install --no-install-recommends "${packages[@]}"
    if "$missing_drogon"; then
        printf 'Apt prerequisites installed; setup remains incomplete until Drogon is installed.\n' >&2
        exit 1
    fi
    if "$public_only"; then
        printf 'Apt prerequisites installed. Run Scripts/build.sh --public-only as your regular user.\n'
    else
        printf 'Apt prerequisites installed. Run Scripts/build.sh as your regular user.\n'
    fi
fi
