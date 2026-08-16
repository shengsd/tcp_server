# ==============================================================================
# [TCP Server] Windows Build Script (PowerShell)
# ==============================================================================
param (
    [string]$Configuration = "Release"
)

Write-Host "==============================================================================" -ForegroundColor Cyan
Write-Host "[TCP Server] Windows Build Script ($Configuration Mode - x64)" -ForegroundColor Cyan
Write-Host "==============================================================================" -ForegroundColor Cyan

# 1. Check/Locate CMake
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmakeCmd) {
    # Check vcpkg download tools cache
    $vcpkgCmake = Get-ChildItem (Join-Path $env:USERPROFILE "vcpkg\downloads\tools") -Recurse -Filter "cmake.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vcpkgCmake) {
        $cmakeDir = Split-Path $vcpkgCmake.FullName
        $env:PATH = "$cmakeDir;$env:PATH"
        $cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
    }
}

if (-not $cmakeCmd) {
    Write-Host "[ERROR] CMake is not found in PATH. Please install CMake: https://cmake.org/download/" -ForegroundColor Red
    exit 1
}

# 2. Locate vcpkg toolchain
$vcpkgToolchain = $null
$possibleVcpkgRoots = @(
    $env:VCPKG_ROOT,
    (Join-Path $env:USERPROFILE "vcpkg"),
    "C:\vcpkg",
    "D:\vcpkg",
    (Join-Path $PSScriptRoot "vcpkg")
)

foreach ($root in $possibleVcpkgRoots) {
    if ($root -and (Test-Path (Join-Path $root "scripts\buildsystems\vcpkg.cmake"))) {
        $vcpkgToolchain = Join-Path $root "scripts\buildsystems\vcpkg.cmake"
        break
    }
}

$cmakeArgs = @("-B", "build", "-S", ".", "-A", "x64")
if ($vcpkgToolchain) {
    Write-Host "[INFO] Using vcpkg toolchain: $vcpkgToolchain" -ForegroundColor Green
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
} else {
    Write-Host "[WARNING] vcpkg toolchain not found. If log4cxx is missing, please run 'setup_vcpkg.bat' first." -ForegroundColor Yellow
}

# 3. Configure
Write-Host "`n[INFO] Running CMake configure (x64)..." -ForegroundColor Green
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] CMake configure failed." -ForegroundColor Red
    exit 1
}

# 4. Build
Write-Host "`n[INFO] Compiling project ($Configuration)..." -ForegroundColor Green
& cmake --build build --config $Configuration -j
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Build failed." -ForegroundColor Red
    exit 1
}

Write-Host "`n==============================================================================" -ForegroundColor Cyan
Write-Host "[SUCCESS] TCP Server built successfully!" -ForegroundColor Green
$exePath = Join-Path "build" "$Configuration\tcp_server_example.exe"
if (-not (Test-Path $exePath)) {
    $exePath = Join-Path "build" "tcp_server_example.exe"
}
Write-Host "[INFO] Executable: $exePath" -ForegroundColor Yellow
Write-Host "==============================================================================" -ForegroundColor Cyan
