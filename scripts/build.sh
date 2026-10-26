#!/usr/bin/env bash
#---------------------------------------------------------------------------------------------
# Build guid-a-tron into guid-a-tron/out/<config>/bin.
#
# Usage: guid-a-tron/scripts/build.sh [--debug] [--test] [-- <extra build args>]
#   --debug   Debug configuration (default Release)
#   --test    run guid-a-tron-tests after building
#
# There is nothing to set up first. The tool links nothing from iTwin: SQLite and nlohmann/json are
# downloaded once (pinned by URL and SHA-256 in cmake/dependencies.cmake, cached in
# third_party/download) and the LZMA SDK is vendored in third_party/lzma. Only a first build needs
# the network.
#
# Env: GUID_A_TRON_DEPS_DIR  relocate the download cache, e.g. to a pre-populated one
#---------------------------------------------------------------------------------------------
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

CONFIG=Release
RUN_TESTS=0
EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --debug) CONFIG=Debug ;;
        --test)  RUN_TESTS=1 ;;
        --) shift; EXTRA=("$@"); break ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

BUILD_DIR="$ROOT/out/$CONFIG"
GENERATOR=()
if command -v ninja > /dev/null; then
    GENERATOR=(-G Ninja)
fi

cmake "${GENERATOR[@]+"${GENERATOR[@]}"}" -S "$ROOT" -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE="$CONFIG" \
      ${GUID_A_TRON_DEPS_DIR:+-DGUID_A_TRON_DEPS_DIR="$GUID_A_TRON_DEPS_DIR"}
cmake --build "$BUILD_DIR" --target guid-a-tron guid-a-tron-tests \
      ${EXTRA[@]+"${EXTRA[@]}"}

echo
echo "built: $BUILD_DIR/bin/guid-a-tron"
if [ "$RUN_TESTS" = 1 ]; then
    echo
    "$BUILD_DIR/bin/guid-a-tron-tests"
fi
