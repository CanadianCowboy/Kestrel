#!/usr/bin/env bash
# Native Linux build, tests, and optional install. Never installs dependencies
# or uses sudo; toolchain/runtime selection stays with CMake and the caller.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: bash scripts/build-linux.sh [options] [-- CMake options...]

  --core-only               Build portable core/tests without Qt
  --build-dir DIR           Build directory (default: <repo>/build-linux)
  --install-prefix DIR      Install desktop app after successful tests
  --help                    Show this help

Examples:
  bash scripts/build-linux.sh
  bash scripts/build-linux.sh -- -DCMAKE_PREFIX_PATH=/path/to/Qt
  bash scripts/build-linux.sh --install-prefix "$HOME/.local"
  bash scripts/build-linux.sh --core-only -- -DKESTREL_ENABLE_CUDA=OFF

CMAKE_BUILD_PARALLEL_LEVEL controls build parallelism (default: 2).
EOF
}

fail() { printf 'Kestrel: %s\n' "$*" >&2; exit 1; }
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build="$repo/build-linux"
prefix=""
core_only=OFF
cmake_options=()

while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --core-only) core_only=ON; shift ;;
        --build-dir|--install-prefix)
            (($# >= 2)) && [[ -n "$2" && "$2" != --* ]] || fail "$1 requires a directory"
            if [[ "$1" == --build-dir ]]; then build="$2"; else prefix="$2"; fi
            shift 2 ;;
        --) shift; cmake_options=("$@"); break ;;
        *) fail "unknown option '$1'; use --help or pass CMake options after --" ;;
    esac
done

[[ "$(uname -s)" == Linux ]] || fail "this helper targets Linux; use CMake directly on other platforms"
[[ "$core_only" != ON || -z "$prefix" ]] || fail "--install-prefix requires a desktop build, not --core-only"
command -v cmake >/dev/null || fail "install CMake 3.24+ and a C++20 compiler first"
command -v ctest >/dev/null || fail "install CTest (provided by CMake) first"

# Relative paths are relative to the caller, even when this script is launched
# outside the repository. No cd into the checkout and no implicit system install.
cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release "${cmake_options[@]}" \
    -DKESTREL_BUILD_TESTS=ON \
    -DKESTREL_BUILD_UI="$([[ "$core_only" == ON ]] && echo OFF || echo ON)" \
    -DKESTREL_REQUIRE_UI="$([[ "$core_only" == ON ]] && echo OFF || echo ON)"
cmake --build "$build" --config Release --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
ctest --test-dir "$build" -C Release --output-on-failure --no-tests=error

if [[ -n "$prefix" ]]; then
    cmake --install "$build" --config Release --prefix "$prefix"
    printf 'Installed Kestrel into %s (Qt and optional model runtimes must remain installed).\n' "$prefix"
else
    printf 'Build and tests passed: %s\n' "$build"
fi
