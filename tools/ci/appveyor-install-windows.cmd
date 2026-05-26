@echo off
REM ChainAPI — AppVeyor Windows install step.
REM
REM Lives in a real .cmd file rather than inline in appveyor.yml because
REM cmd.exe's parsing of `if (...) else (...)` blocks breaks reliably
REM when AppVeyor wraps inline cmd: scripts. This file runs untouched.
REM
REM Required env vars (set by appveyor.yml's `environment:` block):
REM   QT_VERSION, AQT_VERSION, APPVEYOR_REPO_BRANCH

setlocal enableextensions

echo === Install: CMake 4.x via Chocolatey ===
choco upgrade cmake --version=4.0.2 ^
    --installargs "ADD_CMAKE_TO_PATH=System" ^
    --no-progress -y
if errorlevel 1 exit /b 1
call refreshenv
cmake --version
if errorlevel 1 exit /b 1

echo === Install: aqtinstall %AQT_VERSION% ===
python -m pip install --upgrade --quiet "aqtinstall==%AQT_VERSION%"
if errorlevel 1 exit /b 1

echo === Install: Qt %QT_VERSION% (MSVC 2022 64-bit) ===
set "QT_PREFIX=C:\Qt\%QT_VERSION%\msvc2022_64"
set "QT_MARKER=%QT_PREFIX%\mkspecs\qconfig.pri"

if exist "%QT_MARKER%" (
    echo Qt %QT_VERSION% already present from cache.
) else (
    if exist "C:\Qt\%QT_VERSION%" rmdir /s /q "C:\Qt\%QT_VERSION%"
    REM qtwayland and icu are Linux-only. ChainAPI on Windows needs
    REM qtbase + qtdeclarative + qtsvg.
    aqt install-qt windows desktop %QT_VERSION% win64_msvc2022_64 ^
        --outputdir C:\Qt ^
        --archives qtbase qtdeclarative qtsvg
    if errorlevel 1 exit /b 1
)

if not exist "%QT_PREFIX%\lib\cmake\Qt6\Qt6Config.cmake" (
    echo error: Qt6Config.cmake missing under %QT_PREFIX% 1>&2
    exit /b 1
)

echo === Caches ===
if not exist "C:\vcpkg-bincache" mkdir "C:\vcpkg-bincache"

echo === Persist env vars for later steps ===
appveyor SetVariable -Name CMAKE_PREFIX_PATH -Value "%QT_PREFIX%"
appveyor SetVariable -Name VCPKG_DEFAULT_BINARY_CACHE -Value "C:\vcpkg-bincache"

if /i "%APPVEYOR_REPO_BRANCH%" == "main" (
    appveyor SetVariable -Name PRESET -Value "windows-release"
) else (
    appveyor SetVariable -Name PRESET -Value "windows-debug"
)

echo === Done ===
endlocal
exit /b 0
