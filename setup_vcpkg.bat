@echo off
setlocal enabledelayedexpansion

echo ==============================================================================
echo [TCP Server] Windows vcpkg and log4cxx Automated Setup Script
echo ==============================================================================

:: 1. Check if git is available
where git >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Git is not found in PATH. Please install Git for Windows first:
    echo         https://git-scm.com/download/win
    pause
    exit /b 1
)

:: 2. Determine vcpkg installation directory
if defined VCPKG_ROOT (
    set "VCPKG_DIR=%VCPKG_ROOT%"
) else (
    set "VCPKG_DIR=%USERPROFILE%\vcpkg"
)

echo [INFO] Target vcpkg directory: %VCPKG_DIR%

:: 3. Clone vcpkg if not already present
if not exist "%VCPKG_DIR%\.git" (
    echo [INFO] Cloning vcpkg from GitHub...
    git clone https://github.com/microsoft/vcpkg.git "%VCPKG_DIR%"
    if %errorlevel% neq 0 (
        echo [ERROR] Failed to clone vcpkg repository.
        pause
        exit /b 1
    )
) else (
    echo [INFO] vcpkg repository already exists.
)

:: 4. Bootstrap vcpkg
if not exist "%VCPKG_DIR%\vcpkg.exe" (
    echo [INFO] Bootstrapping vcpkg...
    call "%VCPKG_DIR%\bootstrap-vcpkg.bat"
    if %errorlevel% neq 0 (
        echo [ERROR] Failed to bootstrap vcpkg.
        pause
        exit /b 1
    )
) else (
    echo [INFO] vcpkg.exe already bootstrapped.
)

:: 5. Set VCPKG_ROOT environment variable
setx VCPKG_ROOT "%VCPKG_DIR%" >nul 2>&1
set "VCPKG_ROOT=%VCPKG_DIR%"

:: 6. Install log4cxx:x64-windows
echo [INFO] Installing log4cxx:x64-windows via vcpkg (this may take a few minutes)...
"%VCPKG_DIR%\vcpkg.exe" install log4cxx:x64-windows
if %errorlevel% neq 0 (
    echo [ERROR] Failed to install log4cxx.
    pause
    exit /b 1
)

echo ==============================================================================
echo [SUCCESS] vcpkg and log4cxx have been successfully installed!
echo [INFO] VCPKG_ROOT is set to: %VCPKG_DIR%
echo [INFO] Toolchain file: %VCPKG_DIR%\scripts\buildsystems\vcpkg.cmake
echo.
echo You can now run 'build_windows.bat' to build the TCP Server project.
echo ==============================================================================

pause
