#!/bin/sh
# Build deren on POSIX (Linux / WSL / macOS).
# Usage:
#     sh scripts/posix/build.sh                 # Debug + Release
#     sh scripts/posix/build.sh Debug           # one config
#     sh scripts/posix/build.sh Release Clean   # clean rebuild
#
# Build directories: build-debug / build-release in the repo root.

set -eu

root=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)
cd "$root"

configs="Debug Release"
clean=0
for arg in "$@"; do
    case "$arg" in
        Debug|Release) configs=$arg ;;
        Clean|clean) clean=1 ;;
        *) echo "unknown argument: $arg (expected Debug / Release / Clean)" >&2; exit 1 ;;
    esac
done

for config in $configs; do
    dir="build-$(echo "$config" | tr '[:upper:]' '[:lower:]')"
    echo ""
    echo "== configure + build $config -> $dir =="
    if [ "$clean" = 1 ] && [ -d "$dir" ]; then
        rm -rf "$dir"
    fi
    if [ ! -f "$dir/CMakeCache.txt" ]; then
        cmake -S . -B "$dir" -G Ninja "-DCMAKE_BUILD_TYPE=$config"
    fi
    cmake --build "$dir"
done

echo ""
echo "build complete:"
for config in $configs; do
    dir="build-$(echo "$config" | tr '[:upper:]' '[:lower:]')"
    echo "  $dir/deren  ($config)"
done
echo "run with: sh scripts/posix/run.sh"
