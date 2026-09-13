#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: Scripts/clean.sh [--public-only] [--dry-run]

Run CMake's clean target for configured builds in Build and Build/PublicOnly.
--public-only selects only Build/PublicOnly.
--dry-run validates the build directories and prints commands without cleaning.

Preserves CMake caches, extracted dependencies, test fixtures and example Runtime
data. Does not remove directories or provide a database/cache reset.
Stop builds, tests and running backends before cleaning.
EOF
}

public_only=false
dry_run=false
for argument in "$@"; do
    case "$argument" in
        --public-only) public_only=true ;;
        --dry-run) dry_run=true ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$argument" >&2; usage >&2; exit 2 ;;
    esac
done

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
if [[ -L "$repo_root/Build" ]]; then
    printf 'Refusing to clean a symlinked Build directory.\n' >&2
    exit 1
fi

build_dirs=("$repo_root/Build" "$repo_root/Build/PublicOnly")
if "$public_only"; then
    build_dirs=("$repo_root/Build/PublicOnly")
fi

# Validate every selected build before cleaning any of them.
configured=()
for build_dir in "${build_dirs[@]}"; do
    cache="$build_dir/CMakeCache.txt"
    if [[ -L "$build_dir" || -L "$cache" ]]; then
        printf 'Refusing to clean a symlinked build directory or cache: %s\n' "$build_dir" >&2
        exit 1
    fi
    if [[ -e "$build_dir" && ! -d "$build_dir" ]]; then
        printf 'Expected a build directory: %s\n' "$build_dir" >&2
        exit 1
    fi
    if [[ ! -f "$cache" ]]; then
        printf 'Skipping unconfigured build: %s\n' "$build_dir"
        continue
    fi

    source_dir=''
    cache_dir=''
    while IFS='=' read -r key value; do
        case "$key" in
            CMAKE_HOME_DIRECTORY:INTERNAL) source_dir=$value ;;
            CMAKE_CACHEFILE_DIR:INTERNAL) cache_dir=$value ;;
        esac
    done < "$cache"
    if [[ "$source_dir" != "$repo_root" || "$cache_dir" != "$build_dir" ]]; then
        printf 'Refusing to clean a cache belonging to a different source/build directory: %s\n' "$build_dir" >&2
        exit 1
    fi
    configured+=("$build_dir")
done

if (( ${#configured[@]} == 0 )); then
    printf 'No configured builds to clean.\n'
    exit 0
fi
if ! "$dry_run" && ! command -v cmake >/dev/null; then
    printf 'CMake is missing. Run Scripts/install-deps.sh first.\n' >&2
    exit 1
fi

for build_dir in "${configured[@]}"; do
    if "$dry_run"; then
        printf '%q ' cmake --build "$build_dir" --target clean
        printf '\n'
    else
        printf 'Cleaning build outputs: %s\n' "$build_dir"
        cmake --build "$build_dir" --target clean
    fi
done
