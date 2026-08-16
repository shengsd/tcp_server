# ==============================================================================
# [TCP Server] Windows vcpkg and log4cxx Automated Setup Script (PowerShell)
# ==============================================================================

Write-Host "==============================================================================" -ForegroundColor Cyan
Write-Host "[TCP Server] Windows vcpkg and log4cxx Automated Setup Script" -ForegroundColor Cyan
Write-Host "==============================================================================" -ForegroundColor Cyan

# 1. Check Git
$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if (-not $gitCmd) {
    Write-Host "[ERROR] Git is not found in PATH. Please install Git for Windows first: https://git-scm.com/download/win" -ForegroundColor Red
    exit 1
}

# 2. Determine vcpkg directory
$vcpkgDir = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }
Write-Host "[INFO] Target vcpkg directory: $vcpkgDir" -ForegroundColor Green

# 3. Clone vcpkg if needed
if (-not (Test-Path (Join-Path $vcpkgDir ".git"))) {
    Write-Host "[INFO] Cloning vcpkg from GitHub..." -ForegroundColor Green
    git clone https://github.com/microsoft/vcpkg.git "$vcpkgDir"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERROR] Failed to clone vcpkg." -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "[INFO] vcpkg repository already exists." -ForegroundColor Green
}

# 4. Bootstrap vcpkg
$vcpkgExe = Join-Path $vcpkgDir "vcpkg.exe"
if (-not (Test-Path $vcpkgExe)) {
    Write-Host "[INFO] Bootstrapping vcpkg..." -ForegroundColor Green
    $bootstrapBat = Join-Path $vcpkgDir "bootstrap-vcpkg.bat"
    & "$bootstrapBat"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERROR] Failed to bootstrap vcpkg." -ForegroundColor Red
        exit 1
    }
}

# 5. Patch log4cxx port for MSVC compatibility if needed
$portfile = Join-Path $vcpkgDir "ports\log4cxx\portfile.cmake"
if (Test-Path $portfile) {
    $portContent = Get-Content $portfile -Raw
    if ($portContent -notmatch "/* deprecated */") {
        Write-Host "[INFO] Applying MSVC compiler compatibility patches to log4cxx port..." -ForegroundColor Green
        $cleanContent = @'
vcpkg_download_distfile(ARCHIVE
    URLS "https://archive.apache.org/dist/logging/log4cxx/${VERSION}/apache-log4cxx-${VERSION}.tar.gz"
    FILENAME "apache-log4cxx-${VERSION}.tar.gz"
    SHA512 2647b930c3d8d3f55b586943d691f0b40ef1c96276a95bbcb72a49db4130d690703d4f755faee2f5835684587a66ab04a0f715d1e4bfb1ae43da466804e7a879
)

vcpkg_extract_source_archive(
    SOURCE_PATH ARCHIVE "${ARCHIVE}"
)

# Fix MSVC 2015 C2416 error: [[ deprecated("msg") ]] not supported on VS2015
file(GLOB_RECURSE HEADER_FILES "${SOURCE_PATH}/src/main/include/*.h")
foreach(HDR ${HEADER_FILES})
    file(READ "${HDR}" HDR_CONTENT)
    string(REGEX REPLACE "\\[\\[[ \t\r\n]*deprecated[ \t\r\n]*\\([^]]*\\)[ \t\r\n]*\\]\\]" "/* deprecated */" HDR_CONTENT "${HDR_CONTENT}")
    file(WRITE "${HDR}" "${HDR_CONTENT}")
endforeach()

# Fix MSVC 2015 ambiguous symbol PatternConverterList
file(READ "${SOURCE_PATH}/src/main/cpp/rollingpolicybase.cpp" RPB_CONTENT)
string(REPLACE "PatternConverterList RollingPolicyBase::getPatternConverterList" "LOG4CXX_NS::pattern::PatternConverterList RollingPolicyBase::getPatternConverterList" RPB_CONTENT "${RPB_CONTENT}")
file(WRITE "${SOURCE_PATH}/src/main/cpp/rollingpolicybase.cpp" "${RPB_CONTENT}")

# Fix MSVC 2015 unnamed parameter and missing parent namespace in cpp files
file(GLOB_RECURSE CPP_FILES "${SOURCE_PATH}/src/main/cpp/*.cpp")
foreach(CPP ${CPP_FILES})
    file(READ "${CPP}" CPP_CONTENT)
    string(REGEX REPLACE "([a-zA-Z0-9_:]*Pool[ \t\r\n]*)&\\)" "\\1& pool)" CPP_CONTENT "${CPP_CONTENT}")
    if(NOT "${CPP_CONTENT}" MATCHES "using namespace LOG4CXX_NS;")
        string(REGEX REPLACE "(using namespace LOG4CXX_NS::[a-zA-Z0-9_:]*;)" "using namespace LOG4CXX_NS;\n\\1" CPP_CONTENT "${CPP_CONTENT}")
    endif()
    file(WRITE "${CPP}" "${CPP_CONTENT}")
endforeach()

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        qt        LOG4CXX_QT_SUPPORT
        fmt       ENABLE_FMT_LAYOUT
        fmt       ENABLE_FMT_ASYNC
        fmt       VCPKG_LOCK_FIND_PACKAGE_fmt
        mprfa     LOG4CXX_MULTIPROCESS_ROLLING_FILE_APPENDER
)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DLOG4CXX_INSTALL_PDB=OFF # Installing pdbs failed on debug static. So, disable it and let vcpkg_copy_pdbs() do it
        -DLOG4CXX_ENABLE_ESMTP=OFF
        -DLOG4CXX_ENABLE_ODBC=OFF
        -DBUILD_TESTING=OFF
        -DVCPKG_LOCK_FIND_PACKAGE_fmt=${VCPKG_LOCK_FIND_PACKAGE_fmt}
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/log4cxx)

if(VCPKG_TARGET_IS_LINUX OR VCPKG_TARGET_IS_OSX)
    vcpkg_fixup_pkgconfig()
endif()

file(READ "${CURRENT_PACKAGES_DIR}/share/${PORT}/log4cxxConfig.cmake" _contents)
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/log4cxxConfig.cmake"
"include(CMakeFindDependencyMacro)
find_dependency(expat CONFIG)
${_contents}"
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(
    FILE_LIST
        "${SOURCE_PATH}/LICENSE"
        "${SOURCE_PATH}/NOTICE"
)
'@
        [IO.File]::WriteAllText($portfile, $cleanContent)
    }
}

# 6. Set Environment Variable
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", $vcpkgDir, "User")
$env:VCPKG_ROOT = $vcpkgDir

# 7. Install log4cxx
Write-Host "[INFO] Installing log4cxx:x64-windows via vcpkg (this may take a few minutes)..." -ForegroundColor Green
& "$vcpkgExe" install log4cxx:x64-windows
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Failed to install log4cxx." -ForegroundColor Red
    exit 1
}

Write-Host "==============================================================================" -ForegroundColor Cyan
Write-Host "[SUCCESS] vcpkg and log4cxx have been successfully installed!" -ForegroundColor Green
Write-Host "[INFO] VCPKG_ROOT is set to: $vcpkgDir" -ForegroundColor Yellow
Write-Host "[INFO] Toolchain file: $(Join-Path $vcpkgDir 'scripts\buildsystems\vcpkg.cmake')" -ForegroundColor Yellow
Write-Host ""
Write-Host "You can now run 'build_windows.bat' or '.\build_windows.ps1' to build the project." -ForegroundColor Green
Write-Host "==============================================================================" -ForegroundColor Cyan
