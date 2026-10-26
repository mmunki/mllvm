#!/usr/bin/env bash
# ============================================================================
# mllvm -- build script
# ----------------------------------------------------------------------------
#   ./build.sh              build bin/mllvm
#   ./build.sh test         build and run the test suite
#   ./build.sh clean        remove the build artefacts
#   PREFIX=/opt ./build.sh install
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2 -g}"
PREFIX="${PREFIX:-/usr/local}"

build() {
  mkdir -p build bin
  local objects=()
  for src in src/*.c; do
    local obj="build/$(basename "${src%.c}").o"
    # shellcheck disable=SC2086
    $CC $CFLAGS -std=c11 -Wall -Wextra -Wshadow -Wstrict-prototypes \
      -Wmissing-prototypes -c "$src" -o "$obj"
    objects+=("$obj")
  done
  # shellcheck disable=SC2086
  $CC $CFLAGS -o bin/mllvm "${objects[@]}"
  echo "built bin/mllvm"
}

case "${1:-build}" in
  build)
    build
    ;;
  test)
    build
    ./tests/run.sh
    ;;
  clean)
    rm -rf build bin
    ;;
  install)
    build
    install -Dm755 bin/mllvm "$PREFIX/bin/mllvm"
    echo "installed to $PREFIX/bin/mllvm"
    ;;
  *)
    echo "usage: $0 [build|test|clean|install]" >&2
    exit 1
    ;;
esac
