@echo off
REM ChainAPI — AppVeyor Windows vcpkg bootstrap.
REM
REM Standalone .cmd because cmd.exe's `if (...)` parsing breaks inside
REM AppVeyor's inline cmd: scripts.
REM
REM Required env vars set by appveyor.yml:
REM   APPVEYOR_BUILD_FOLDER, VCPKG_COMMIT
REM
REM Output env vars (set via stdout for the YAML caller to capture):
REM   VCPKG_ROOT
REM   VCPKG_BINARY_SOURCES

setlocal enableextensions

set "VCPKG_ROOT=%APPVEYOR_BUILD_FOLDER%\vcpkg"

if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    git clone https://github.com/microsoft/vcpkg.git "%VCPKG_ROOT%"
    if errorlevel 1 exit /b 1
    git -C "%VCPKG_ROOT%" checkout "%VCPKG_COMMIT%"
    if errorlevel 1 exit /b 1
    call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics
    if errorlevel 1 exit /b 1
) else (
    echo vcpkg already present from cache.
)

endlocal & set "VCPKG_ROOT=%APPVEYOR_BUILD_FOLDER%\vcpkg" & set "VCPKG_BINARY_SOURCES=clear;files,%VCPKG_DEFAULT_BINARY_CACHE%,readwrite"
exit /b 0
