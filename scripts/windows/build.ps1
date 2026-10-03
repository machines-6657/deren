# Build deren (Windows). Requires the MSYS2 clang64 toolchain on
# PATH (run scripts/windows/setup.sh first, or start an MSYS2 clang64 shell).
# Usage:
#     powershell -ExecutionPolicy Bypass -File scripts/windows/build.ps1
#     powershell -ExecutionPolicy Bypass -File scripts/windows/build.ps1 -Type Debug
#     powershell -ExecutionPolicy Bypass -File scripts/windows/build.ps1 -Type Release -Clean
#
# Build directories: build-debug-clang64 / build-release-clang64 in the repo
# root (not committed). A full Debug + Release pass is the default.

param(
    [ValidateSet('Debug', 'Release')]
    [string[]]$Type = @('Debug', 'Release'),
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot | Split-Path -Parent | Split-Path -Parent
Push-Location $root

# ---- make clang64 reachable (idempotent) ----
$msys = 'C:\msys64'
if (-not (Get-Command clang++ -ErrorAction SilentlyContinue)) {
    $clang64 = Join-Path $msys 'clang64\bin'
    if (Test-Path (Join-Path $clang64 'clang++.exe')) {
        $env:PATH = "$clang64;$env:PATH"
        Write-Host "added $clang64 to PATH"
    }
}
if (-not (Get-Command clang++ -ErrorAction SilentlyContinue)) {
    Write-Error 'clang++ not found. Run scripts/windows/setup.sh (MSYS2 clang64) or add clang64\bin to PATH.'
}

# ---- Vulkan SDK: cmake's find_package(Vulkan) reads VULKAN_SDK ----
if (-not $env:VULKAN_SDK) {
    $sdk = Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($sdk) {
        $env:VULKAN_SDK = $sdk.FullName
        Write-Host "VULKAN_SDK=$($env:VULKAN_SDK)"
    }
}

foreach ($config in $Type) {
    $dir = "build-$($config.ToLowerInvariant())-clang64"
    Write-Host ''
    Write-Host "== configure + build $config -> $dir =="

    if ($Clean -and (Test-Path $dir)) {
        Remove-Item -Recurse -Force $dir
    }
    if (-not (Test-Path (Join-Path $dir 'CMakeCache.txt'))) {
        cmake -S . -B $dir -G Ninja "-DCMAKE_BUILD_TYPE=$config"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    cmake --build $dir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Pop-Location
Write-Host ''
Write-Host 'build complete:'
foreach ($config in $Type) {
    $dir = Join-Path $root "build-$($config.ToLowerInvariant())-clang64"
    Write-Host "  $dir\deren.exe  ($config)"
}
Write-Host 'run with:  powershell -ExecutionPolicy Bypass -File scripts/windows/run.ps1'
