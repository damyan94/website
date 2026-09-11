#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: Scripts/build.sh [--public-only] [--release] [--jobs N] [-- CMAKE_OPTIONS...]

Configure and build the full application in Build (Debug, two jobs by default).
--public-only uses Build/PublicOnly and disables all optional modules.
--release selects Release; --jobs overrides CMAKE_BUILD_PARALLEL_LEVEL or two jobs.
Additional CMake configure options may follow --, for example:
  Scripts/build.sh -- -DCMAKE_PREFIX_PATH=/opt/drogon

Felis tests are enabled. FELIS_USE_STD_FORMAT defaults to OFF for GCC 11/12 support;
pass -- -DFELIS_USE_STD_FORMAT=ON on a toolchain with the required library support.
Existing CMake caches/generators are reused; new builds use Ninja.
EOF
}

public_only=false
build_type=Debug
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-2}
cmake_options=()
while (( $# )); do
    case "$1" in
        --public-only) public_only=true; shift ;;
        --release) build_type=Release; shift ;;
        --jobs)
            if (( $# < 2 )); then printf '%s\n' '--jobs requires a positive integer.' >&2; exit 2; fi
            jobs=$2; shift 2 ;;
        --) shift; cmake_options=("$@"); break ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    printf '%s\n' '--jobs / CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer.' >&2
    exit 2
fi

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if ! command -v cmake >/dev/null; then
    printf 'CMake is missing. Run Scripts/install-deps.sh first.\n' >&2
    exit 1
fi
if [[ ! -f "$repo_root/Dependencies/FelisFramework/CMakeLists.txt" ]]; then
    printf 'Dependencies/FelisFramework is missing. Populate the existing submodule before building.\n' >&2
    exit 1
fi

build_dir="$repo_root/Build"
features=ON
if "$public_only"; then
    build_dir+=/PublicOnly
    features=OFF
fi
configure=(-S "$repo_root" -B "$build_dir"
    "-DCMAKE_BUILD_TYPE=$build_type"
    "-DWEBSITE_ENABLE_ACCOUNTS=$features"
    "-DWEBSITE_ENABLE_CONTENT_EDITOR=$features"
    "-DWEBSITE_ENABLE_RESERVATIONS=$features"
    -DFELIS_USE_STD_FORMAT=OFF -DFELIS_BUILD_TEST=ON)
if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    if ! command -v ninja >/dev/null; then
        printf 'Ninja is missing. Run Scripts/install-deps.sh first.\n' >&2
        exit 1
    fi
    configure+=(-G Ninja)
    # Preserve support for this checkout's extracted PostgreSQL without requiring
    # those machine-specific paths on a fresh system installation.
    local_pg="$repo_root/Build/LocalPostgres/root/usr"
    if ! "$public_only" && [[ -f "$local_pg/include/postgresql/libpq-fe.h" &&
        -f "$local_pg/lib/x86_64-linux-gnu/libpq.so" ]]; then
        configure+=("-DPostgreSQL_INCLUDE_DIR=$local_pg/include/postgresql"
            "-DPostgreSQL_LIBRARY=$local_pg/lib/x86_64-linux-gnu/libpq.so")
    fi
fi

cmake "${configure[@]}" "${cmake_options[@]}"
cmake --build "$build_dir" --config "$build_type" --parallel "$jobs"
