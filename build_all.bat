@echo off
REM ============================================================
REM NetLeaf - Windows build (MSVC / Visual Studio)
REM This script ONLY builds Windows targets. Linux / macOS / Android are
REM handled by their own dedicated scripts and are never invoked from here.
REM
REM Usage: build_all.bat [x64|x86|arm64|all]   (default: all)
REM         amd64 is an alias of x64
REM
REM Visual Studio is located automatically through vswhere, so a plain
REM command prompt (no Developer Command Prompt) is enough.
REM For LLVM-MinGW instead of MSVC use: build_all-Clang.bat
REM ============================================================

setlocal enabledelayedexpansion
set "VERSION=3.0.0"
set "SRC_DIR=%~dp0"
set "SRC_DIR=%SRC_DIR:~0,-1%"

REM ---------- parse argument ----------
set "ARCH=%~1"
if not defined ARCH set "ARCH=all"
if /i "%ARCH%"=="amd64" set "ARCH=x64"

set "BUILD_X64=0"
set "BUILD_X86=0"
set "BUILD_ARM64=0"
if /i "%ARCH%"=="all"   set "BUILD_X64=1"
if /i "%ARCH%"=="all"   set "BUILD_X86=1"
if /i "%ARCH%"=="all"   set "BUILD_ARM64=1"
if /i "%ARCH%"=="x64"   set "BUILD_X64=1"
if /i "%ARCH%"=="x86"   set "BUILD_X86=1"
if /i "%ARCH%"=="arm64" set "BUILD_ARM64=1"
if "%BUILD_X64%%BUILD_X86%%BUILD_ARM64%"=="000" (
    echo [ERROR] Unknown architecture: %ARCH%
    echo Usage: build_all.bat [x64^|x86^|arm64^|all]
    exit /b 1
)

REM ---------- locate Visual Studio ----------
if not defined VSWHERE set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_INSTALL="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
)

set "VS_GEN="
if defined VS_INSTALL if not "%VS_INSTALL%"=="" (
    echo %VS_INSTALL% | findstr /C:"2022" >nul
    if not errorlevel 1 set "VS_GEN=Visual Studio 17 2022"
)
if defined VS_INSTALL if not defined VS_GEN if not "%VS_INSTALL%"=="" (
    echo %VS_INSTALL% | findstr /C:"2019" >nul
    if not errorlevel 1 set "VS_GEN=Visual Studio 16 2019"
)
if not defined VS_GEN if defined VSCMD_VER set "VS_GEN=NMake Makefiles"
if not defined VS_GEN where cl.exe >nul 2>nul && set "VS_GEN=NMake Makefiles"
if not defined VS_GEN (
    echo [ERROR] Visual Studio ^(MSVC toolset^) not found.
    echo         Install "Visual Studio Build Tools" with the C++ workload, or use
    echo         the LLVM-MinGW based script instead: build_all-Clang.bat
    exit /b 1
)

echo ========================================
echo   NetLeaf v%VERSION% - Windows (MSVC)
echo ========================================
echo Source   : %SRC_DIR%
echo VS       : %VS_INSTALL%
echo Generator: %VS_GEN%
echo Target   : %ARCH%
echo Output   : releases\
echo.

if not exist "%SRC_DIR%\releases" mkdir "%SRC_DIR%\releases"

REM BUILD_TLS(tls2) conflicts with BUILD_TLS3 inside one build tree; pick TLS3.
set "NL_OPTS=-DBUILD_TESTS=ON -DBUILD_EXAMPLES=ON -DBUILD_TLS=OFF -DBUILD_TLS3=ON -DBUILD_LANG=ON -DBUILD_MQTT=ON -DBUILD_MQTT_SERVER=ON"

if "%BUILD_X64%"=="1"   call :build_arch x64   x64   && call :package x64
if "%BUILD_X86%"=="1"   call :build_arch x86   Win32 && call :package x86
if "%BUILD_ARM64%"=="1" call :build_arch arm64 ARM64 && call :package arm64

echo ========================================
echo   Windows (MSVC) builds completed
echo ========================================
echo Packages in: %SRC_DIR%\releases\
endlocal
exit /b 0

REM ============================================================
REM :build_arch  <pack_arch>  <cmake_platform>
REM ============================================================
:build_arch
set "PACK_ARCH=%~1"
set "CM_PLATFORM=%~2"
set "BDIR=%SRC_DIR%\build_msvc_%PACK_ARCH%"
echo.
echo ----------------------------------------
echo [Build] Windows %PACK_ARCH% ^(MSVC, -A %CM_PLATFORM%^)
echo ----------------------------------------
if exist "%BDIR%" rmdir /s /q "%BDIR%"
mkdir "%BDIR%"
pushd "%BDIR%"
set "GEN_ARGS="
if /i not "%VS_GEN%"=="NMake Makefiles" set "GEN_ARGS=-A %CM_PLATFORM%"
cmake -G "%VS_GEN%" %GEN_ARGS% %NL_OPTS% "%SRC_DIR%"
if errorlevel 1 (
    echo [ERROR] CMake configure failed for %PACK_ARCH%
    popd
    exit /b 1
)
if /i "%VS_GEN%"=="NMake Makefiles" (
    cmake --build . --config Release
) else (
    cmake --build . --config Release --parallel %NUMBER_OF_PROCESSORS%
)
if errorlevel 1 (
    echo [ERROR] Build failed for %PACK_ARCH%
    popd
    exit /b 1
)
popd
echo [OK] Windows %PACK_ARCH% built
exit /b 0

REM ============================================================
REM :package  <pack_arch>
REM ============================================================
:package
set "PACK_ARCH=%~1"
set "BDIR=%SRC_DIR%\build_msvc_%PACK_ARCH%"
set "PKG=%BDIR%\pkg_out"
echo [Pack] %PACK_ARCH% ...
if exist "%PKG%" rmdir /s /q "%PKG%"
mkdir "%PKG%"
copy /Y "%BDIR%\bin\Release\*.dll" "%PKG%\" >nul 2>nul
copy /Y "%BDIR%\bin\*.dll" "%PKG%\" >nul 2>nul
copy /Y "%BDIR%\lib\Release\*.lib" "%PKG%\" >nul 2>nul
copy /Y "%BDIR%\lib\*.lib" "%PKG%\" >nul 2>nul
copy /Y "%SRC_DIR%\include\*.h" "%PKG%\" >nul 2>nul
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%PKG%\*' -DestinationPath '%SRC_DIR%\releases\NetLeaf-%VERSION%-windows-%PACK_ARCH%-msvc.zip' -Force"
rmdir /s /q "%PKG%"
echo [OK] %SRC_DIR%\releases\NetLeaf-%VERSION%-windows-%PACK_ARCH%-msvc.zip
exit /b 0
