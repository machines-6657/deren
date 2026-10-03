#!/bin/sh
# POSIX (Linux / WSL / macOS) environment setup for deren.
#
# Run from anywhere:
#     sh scripts/posix/setup.sh
#
# The glTF parser (fastgltf + simdjson), texture decoder (stb_image) and VMA
# are vendored under third_party/, so the only system dependencies are the
# Vulkan headers/loader, GLFW, GLM and toml++ (and a C++23 module-capable
# compiler - clang 18+ or gcc 14+). This script detects the distro, reports
# what is missing and installs it (apt on Debian/Ubuntu, dnf on Fedora,
# pacman on Arch, brew on macOS); installs are optional when you prefer to
# use your own package manager.
#
# Afterwards:
#     sh scripts/posix/build.sh              # Debug + Release
#     sh scripts/posix/run.sh                # run the demo
#     sh scripts/posix/run.sh <model.glb> gui

set -eu

root=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)
cd "$root"

echo "== deren: POSIX environment setup =="

# ---------- compiler ----------
have_cxx=""
for cxx in clang++ g++ c++ c++23; do
    if command -v "$cxx" >/dev/null 2>&1; then
        have_cxx=$cxx
        break
    fi
done
if [ -z "$have_cxx" ]; then
    echo "error: no C++ compiler found (need clang 18+ / gcc 14+ for C++23 modules)." >&2
    exit 1
fi
echo "compiler: $have_cxx"
echo "note: C++23 modules need a recent clang (>= 18) or gcc (>= 14)."

# ---------- detect package manager / distro ----------
install_cmd=""
pkg_glfw=""
pkg_glm=""
pkg_toml=""
pkg_vulkan=""
pkg_cmake=""
pkg_ninja=""

if command -v apt-get >/dev/null 2>&1; then
    install_cmd="sudo apt-get install -y"
    pkg_glfw="libglfw3-dev"
    pkg_glm="libglm-dev"
    pkg_toml="libtomlplusplus-dev"     # Debian bookworm+/Ubuntu 23.04+; older: build toml++ yourself
    pkg_vulkan="libvulkan-dev vulkan-tools"
    pkg_cmake="cmake"
    pkg_ninja="ninja-build"
    echo "package manager: apt (Debian/Ubuntu)"
elif command -v dnf >/dev/null 2>&1; then
    install_cmd="sudo dnf install -y"
    pkg_glfw="glfw-devel"
    pkg_glm="glm-devel"
    pkg_toml="tomlplusplus-devel"
    pkg_vulkan="vulkan-devel vulkan-tools"
    pkg_cmake="cmake"
    pkg_ninja="ninja-build"
    echo "package manager: dnf (Fedora)"
elif command -v pacman >/dev/null 2>&1; then
    install_cmd="sudo pacman -S --needed --noconfirm"
    pkg_glfw="glfw"
    pkg_glm="glm"
    pkg_toml="tomlplusplus"
    pkg_vulkan="vulkan-icd-loader vulkan-headers vulkan-tools"
    pkg_cmake="cmake"
    pkg_ninja="ninja"
    echo "package manager: pacman (Arch)"
elif command -v brew >/dev/null 2>&1; then
    install_cmd="brew install"
    pkg_glfw="glfw"
    pkg_glm="glm"
    pkg_toml="tomlplusplus"
    pkg_vulkan="vulkan-headers vulkan-loader vulkan-tools"
    pkg_cmake="cmake"
    pkg_ninja="ninja"
    echo "package manager: brew (macOS)"
else
    echo "note: no supported package manager detected - install these yourself:"
    echo "  glfw3, glm, tomlplusplus, Vulkan headers/loader, cmake, ninja"
fi

# ---------- check what is missing ----------
missing=""
for tool in cmake ninja; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
check_hdr() { # check_hdr <name> <header file>
    printf '#include <%s>\nint main(){return 0;}\n' "$2" | \
        ${CXX:-$have_cxx} -x c++ -fsyntax-only - 2>/dev/null
}

if [ -n "$install_cmd" ]; then
    if ! check_hdr glfw "GLFW/glfw3.h"; then missing="$missing $pkg_glfw"; fi
    if ! check_hdr glm "glm/glm.hpp"; then missing="$missing $pkg_glm"; fi
    if ! check_hdr toml "toml++/toml.hpp"; then missing="$missing $pkg_toml"; fi
    if ! check_hdr vulkan "vulkan/vulkan.h"; then missing="$missing $pkg_vulkan"; fi
fi

if [ -n "$missing" ]; then
    echo "missing system dependencies:$missing"
    if [ -n "$install_cmd" ]; then
        echo "installing with: $install_cmd$missing"
        # shellcheck disable=SC2086
        $install_cmd $missing
    else
        echo "install them with your package manager, then rerun this script."
        exit 1
    fi
else
    echo "system dependencies: all present"
fi

# ---------- default config ----------
if [ ! -f config.toml ]; then
    cp config.example.toml config.toml
    echo "created config.toml from config.example.toml (edit to taste)"
else
    echo "config.toml: already present"
fi

echo ""
echo "environment ready. Next:"
echo "  sh scripts/posix/build.sh      # Debug + Release"
echo "  sh scripts/posix/run.sh        # run the demo"
