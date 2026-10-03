#!/bin/sh
# Windows (MSYS2) environment setup for deren.
#
# Run this from an MSYS2 shell (any of mingw64/clang64/msys terminals work -
# the script switches to the clang64 toolchain itself):
#     sh scripts/windows/setup.sh
#
# What it does:
#   1. Locates the MSYS2 clang64 toolchain (clang / cmake / ninja must be
#      reachable - adds C:\msys64\clang64\bin to PATH for the current shell).
#   2. Installs missing MSYS2 packages via pacman (clang, cmake, ninja, glfw,
#      glm, fastgltf, tomlplusplus, vulkan loader/headers/tools).
#   3. Detects the Vulkan SDK (env VULKAN_SDK or C:\VulkanSDK\<ver>) - needed
#      for the Vulkan headers, VMA (vma/vk_mem_alloc.h) and glslc.
#   4. Copies config.example.toml to config.toml when missing.
#
# Afterwards build with the PowerShell script (or cmake directly):
#     powershell -ExecutionPolicy Bypass -File scripts/windows/build.ps1

set -eu

root=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)
cd "$root"

echo "== deren: Windows (MSYS2) environment setup =="

# ---------- 1. clang64 toolchain on PATH ----------
msys_root=""
if command -v cygpath >/dev/null 2>&1 && command -v uname >/dev/null 2>&1; then
    # running inside MSYS2: find the clang64 prefix
    for cand in /clang64 /c/msys64/clang64 "$LOCALAPPDATA/Programs/msys64/clang64"; do
        if [ -d "$cand" ]; then
            msys_root=$cand
            break
        fi
    done
fi
if [ -n "$msys_root" ] && ! echo "$PATH" | tr ':' '\n' | grep -qx "$msys_root/bin"; then
    export PATH="$msys_root/bin:$PATH"
    echo "added $msys_root/bin to PATH"
fi

missing=""
for tool in clang clang++ cmake ninja pacman; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        missing="$missing $tool"
    fi
done
if [ -n "$missing" ]; then
    echo "error: not running inside MSYS2 with clang64 reachable; missing:$missing" >&2
    echo "       start an MSYS2 'clang64' shell, or add <msys>/clang64/bin to PATH." >&2
    exit 1
fi
echo "toolchain: $(command -v clang++) / cmake $(cmake --version | head -1 | awk '{print $3}')"

# ---------- 2. MSYS2 packages (clang64) ----------
# Simpler to always sync the package list; pacman -Q returns 1 for missing,
# so query each package individually and install the missing ones.
# fastgltf/simdjson/stb/VMA are vendored under third_party/ (no packages needed).
pkg() { pacman -Q "mingw-w64-clang-x86_64-$1" >/dev/null 2>&1; }

need=""
for p in clang cmake ninja glfw glm tomlplusplus vulkan-loader vulkan-headers; do
    if ! pkg "$p"; then
        need="$need mingw-w64-clang-x86_64-$p"
    fi
done

if [ -n "$need" ]; then
    echo "installing missing MSYS2 packages:$need"
    # -S --needed keeps already-installed packages untouched; --noconfirm skips prompts
    pacman -S --needed --noconfirm $need
else
    echo "MSYS2 packages: all present"
fi

# ---------- 3. Vulkan SDK (headers, glslc; VMA is vendored under third_party/vma) ----------
vulkan_dir="${VULKAN_SDK:-}"
if [ -z "$vulkan_dir" ] || [ ! -d "$vulkan_dir/Include" ]; then
    # fall back to C:\VulkanSDK\<latest>
    for d in /c/VulkanSDK/*/; do
        [ -d "${d}Include" ] && vulkan_dir=$(cygpath -u "${d%/}") && break
    done
fi
if [ -z "$vulkan_dir" ] || [ ! -d "$vulkan_dir/Include" ]; then
    echo "error: Vulkan SDK not found. Install the LunarG Vulkan SDK"
    echo "       (https://vulkan.lunarg.com/sdk/home#windows) - glslc comes from it."
    exit 1
fi
echo "Vulkan SDK: $vulkan_dir"

# ---------- 4. default config ----------
if [ ! -f config.toml ]; then
    cp config.example.toml config.toml
    echo "created config.toml from config.example.toml (edit to taste)"
else
    echo "config.toml: already present"
fi

echo ""
echo "environment ready. Next:"
echo "  powershell -ExecutionPolicy Bypass -File scripts/windows/build.ps1   # Debug+Release"
echo "  powershell -ExecutionPolicy Bypass -File scripts/windows/run.ps1     # run the demo"
