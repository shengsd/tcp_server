@echo off
setlocal enabledelayedexpansion

echo ==============================================================================
echo [TCP Server] Windows Build Script
echo ==============================================================================

:: 1. Locate CMake (PATH or vcpkg tools cache)
where cmake >nul 2>&1
if %errorlevel% neq 0 (
    for /d %%D in ("%USERPROFILE%\vcpkg\downloads\tools\cmake-*-windows\cmake-*-windows-x86_64\bin" "%VCPKG_ROOT%\downloads\tools\cmake-*-windows\cmake-*-windows-x86_64\bin") do (
        if exist "%%D\cmake.exe" (
            set "PATH=%%D;!PATH!"
        )
    )
)

where cmake >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] CMake is not found in PATH. Please install CMake:
    echo         https://cmake.org/download/
    pause
    exit /b 1
)

:: 2. Locate vcpkg toolchain
set "VCPKG_TOOLCHAIN="
if defined VCPKG_ROOT (
    if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
        set "VCPKG_TOOLCHAIN=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
    )
)

if not defined VCPKG_TOOLCHAIN (
    if exist "%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake" (
        set "VCPKG_TOOLCHAIN=%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake"
    ) else if exist "C:\vcpkg\scripts\buildsystems\vcpkg.cmake" (
        set "VCPKG_TOOLCHAIN=C:\vcpkg\scripts\buildsystems\vcpkg.cmake"
    )
)

if not defined VCPKG_TOOLCHAIN (
    echo [WARNING] vcpkg toolchain file not automatically found.
    echo [INFO]    If build fails due to missing log4cxx, please run 'setup_vcpkg.bat' first.
    set "CMAKE_TOOLCHAIN_OPT="
) else (
    echo [INFO] Using vcpkg toolchain: !VCPKG_TOOLCHAIN!
    set "CMAKE_TOOLCHAIN_OPT=-DCMAKE_TOOLCHAIN_FILE=!VCPKG_TOOLCHAIN!"
)

:: 3. Configure project with CMake (Target x64 architecture)
echo.
echo [INFO] Configuring CMake build (x64 architecture)...
if not exist build (
    mkdir build
)

cmake -B build -S . -A x64 %CMAKE_TOOLCHAIN_OPT%
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] CMake configuration failed.
    if not defined VCPKG_TOOLCHAIN (
        echo [HINT] Please run 'setup_vcpkg.bat' to install required log4cxx dependencies.
    )
    pause
    exit /b 1
)

:: 4. Build project
echo.
echo [INFO] Building TCP Server (Release mode)...
cmake --build build --config Release -j
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

echo.
echo ==============================================================================
echo [SUCCESS] TCP Server built successfully!
echo [INFO] Executable can be found at:
if exist "build\Release\tcp_server_example.exe" (
    echo        build\Release\tcp_server_example.exe
) else (
    echo        build\tcp_server_example.exe
)
echo ==============================================================================
pause
