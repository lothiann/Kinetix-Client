@echo off
setlocal

rem Kinetix build helper (Ninja, x64-Release).
rem
rem Usage:
rem   scripts/build.bat              incremental build (default)
rem   scripts/build.bat build        incremental build      (-b, --build)
rem   scripts/build.bat rebuild      clean + full rebuild    (-r, --rebuild)
rem   scripts/build.bat clean        clean only              (-c, --clean)
rem
rem The script initializes the Visual Studio environment itself, so it can be
rem run from a plain cmd.exe / PowerShell / Git Bash (via cmd //c).

set "BUILD_DIR=%~dp0../out/build/x64-Release"
set "VCVARS="
set "NINJA="

rem --- locate vcvars64.bat ---------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%/Microsoft Visual Studio/Installer/vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if defined VSPATH if exist "%VSPATH%/VC/Auxiliary/Build/vcvars64.bat" set "VCVARS=%VSPATH%/VC/Auxiliary/Build/vcvars64.bat"
if not defined VCVARS if exist "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat" set "VCVARS=C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat"
if not defined VCVARS if exist "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat" set "VCVARS=C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat"

rem --- locate ninja.exe ------------------------------------------------------
if exist "C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" set "NINJA=C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
if not defined NINJA if exist "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" set "NINJA=C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
if not defined NINJA (
    for /f "delims=" %%i in ('where ninja 2^>nul') do if not defined NINJA set "NINJA=%%i"
)

rem --- validate --------------------------------------------------------------
if not defined VCVARS (
    echo [build] ERROR: vcvars64.bat not found. Install Visual Studio C++ tools.
    exit /b 1
)
if not defined NINJA (
    echo [build] ERROR: ninja.exe not found.
    exit /b 1
)
if not exist "%BUILD_DIR%/build.ninja" (
    echo [build] ERROR: build directory is not configured: "%BUILD_DIR%"
    echo [build] Configure CMake for x64-Release first.
    exit /b 1
)

rem --- mode ------------------------------------------------------------------
set "MODE=%~1"
if "%MODE%"=="" set "MODE=build"
if /I "%MODE%"=="-b" set "MODE=build"
if /I "%MODE%"=="--build" set "MODE=build"
if /I "%MODE%"=="-r" set "MODE=rebuild"
if /I "%MODE%"=="--rebuild" set "MODE=rebuild"
if /I "%MODE%"=="-c" set "MODE=clean"
if /I "%MODE%"=="--clean" set "MODE=clean"

if /I "%MODE%"=="build" goto do_build
if /I "%MODE%"=="rebuild" goto do_rebuild
if /I "%MODE%"=="clean" goto do_clean

echo [build] ERROR: unknown mode "%MODE%". Use: build ^| rebuild ^| clean
exit /b 1

rem --- run -------------------------------------------------------------------
:do_clean
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [build] ERROR: failed to initialize Visual Studio environment.
    exit /b 1
)
echo [build] Cleaning "%BUILD_DIR%"...
"%NINJA%" -C "%BUILD_DIR%" -t clean
if errorlevel 1 (
    echo [build] CLEAN FAILED.
    exit /b 1
)
echo [build] CLEAN OK.
exit /b 0

:do_rebuild
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [build] ERROR: failed to initialize Visual Studio environment.
    exit /b 1
)
echo [build] Rebuilding (clean + build) "%BUILD_DIR%"...
"%NINJA%" -C "%BUILD_DIR%" -t clean
if errorlevel 1 (
    echo [build] CLEAN FAILED.
    exit /b 1
)
goto do_build_env_ready

:do_build
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [build] ERROR: failed to initialize Visual Studio environment.
    exit /b 1
)

:do_build_env_ready
echo [build] Building "%BUILD_DIR%"...
"%NINJA%" -C "%BUILD_DIR%"
if errorlevel 1 (
    echo [build] BUILD FAILED.
    exit /b 1
)
echo [build] BUILD OK.
exit /b 0