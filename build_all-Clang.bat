@echo off
REM ============================================================
REM NetLeaf - Windows build (LLVM-MinGW / Clang)
REM This script ONLY builds Windows targets. It does not touch
REM Linux / macOS / Android. No other script is invoked.
REM
REM Usage: build_all-Clang.bat [x64|x86|arm64|all]   (default: all)
REM         amd64 is an alias of x64
REM
REM Compiler lookup order:
REM   1) LLVM_MINGW  env var (points at the bin directory)
REM   2) CC          env var (full path of a *-w64-mingw32-clang.exe)
REM   3) PATH        (x86_64/i686/aarch64 -w64-mingw32-clang.exe)
REM   4) common install directories
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
    echo Usage: build_all-Clang.bat [x64^|x86^|arm64^|all]
    exit /b 1
)

REM ---------- locate LLVM-MinGW ----------
set "LLVM_MINGW="
if defined LLVM_MINGW if not exist "%LLVM_MINGW%\x86_64-w64-mingw32-clang.exe" set "LLVM_MINGW="
if not defined LLVM_MINGW if defined CC (
    for %%I in ("%CC%") do if exist "%%~dpIx86_64-w64-mingw32-clang.exe" set "LLVM_MINGW=%%~dpI"
)
if not defined LLVM_MINGW for /f "delims=" %%I in ('where x86_64-w64-mingw32-clang.exe 2^>nul') do if not defined LLVM_MINGW set "LLVM_MINGW=%%~dpI"
if not defined LLVM_MINGW for /f "delims=" %%I in ('where i686-w64-mingw32-clang.exe 2^>nul') do if not defined LLVM_MINGW set "LLVM_MINGW=%%~dpI"
if not defined LLVM_MINGW for /f "delims=" %%I in ('where aarch64-w64-mingw32-clang.exe 2^>nul') do if not defined LLVM_MINGW set "LLVM_MINGW=%%~dpI"
REM fall back to well-known install locations derived from environment vars only
for %%D in (
    "%ProgramFiles%\llvm-mingw\bin"
    "%ProgramFiles(x86)%\llvm-mingw\bin"
    "%LOCALAPPDATA%\llvm-mingw\bin"
    "%USERPROFILE%\llvm-mingw\bin"
) do (
    if not defined LLVM_MINGW if exist "%%~D\x86_64-w64-mingw32-clang.exe" set "LLVM_MINGW=%%~D"
)
if not defined LLVM_MINGW (
    echo [ERROR] LLVM-MinGW not found.
    echo         Put its bin directory on PATH, or set:
    echo           set LLVM_MINGW=^<path\to\llvm-mingw^>\bin
    exit /b 1
)
if "!LLVM_MINGW:~-1!"=="\" set "LLVM_MINGW=!LLVM_MINGW:~0,-1!"
set "PATH=%LLVM_MINGW%;%PATH%"

REM ---------- locate make ----------
set "MAKE_CMD="
if exist "%LLVM_MINGW%\mingw32-make.exe" set "MAKE_CMD=%LLVM_MINGW%\mingw32-make.exe"
if not defined MAKE_CMD for /f "delims=" %%I in ('where mingw32-make.exe 2^>nul') do if not defined MAKE_CMD set "MAKE_CMD=%%I"
if not defined MAKE_CMD for /f "delims=" %%I in ('where make.exe 2^>nul') do if not defined MAKE_CMD set "MAKE_CMD=%%I"
if not defined MAKE_CMD (
    echo [ERROR] mingw32-make / make not found. LLVM-MinGW package normally ships mingw32-make.exe.
    exit /b 1
)

echo ========================================
echo   NetLeaf v%VERSION% - Windows (LLVM-MinGW)
echo ========================================
echo Source : %SRC_DIR%
echo Compiler: %LLVM_MINGW%
echo Make    : %MAKE_CMD%
echo Target  : %ARCH%
echo Output  : releases\
echo.

if not exist "%SRC_DIR%\releases" mkdir "%SRC_DIR%\releases"

REM BUILD_TLS(tls2) conflicts with BUILD_TLS3 inside one build tree; pick TLS3.
set "NL_OPTS=-DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DBUILD_EXAMPLES=ON -DBUILD_TLS=OFF -DBUILD_TLS3=ON -DBUILD_LANG=ON -DBUILD_MQTT=ON -DBUILD_MQTT_SERVER=ON"

if "%BUILD_X64%"=="1"   call :build_arch x64   x86_64 && call :package x64
if "%BUILD_X86%"=="1"   call :build_arch x86   i686  && call :package x86
if "%BUILD_ARM64%"=="1" call :build_arch arm64 aarch64 && call :package arm64

echo ========================================
echo   Windows (LLVM-MinGW) builds completed
echo ========================================
echo Packages in: %SRC_DIR%\releases\
endlocal
exit /b 0

REM ============================================================
REM :build_arch  <arch>  <triplet>
REM ============================================================
:build_arch
set "PACK_ARCH=%~1"
set "TRIPLET=%~2"
set "CC_EXE=%LLVM_MINGW%\%TRIPLET%-w64-mingw32-clang.exe"
set "AR_EXE=%LLVM_MINGW%\%TRIPLET%-w64-mingw32-ar.exe"
set "RANLIB_EXE=%LLVM_MINGW%\%TRIPLET%-w64-mingw32-ranlib.exe"
if not exist "%CC_EXE%" (
    echo [SKIP] %PACK_ARCH%: compiler missing ^(%CC_EXE%^)
    exit /b 1
)
set "BDIR=%SRC_DIR%\build_mingw_%PACK_ARCH%"
echo.
echo ----------------------------------------
echo [Build] Windows %PACK_ARCH% ^(MinGW Makefiles^)
echo ----------------------------------------
if exist "%BDIR%" rmdir /s /q "%BDIR%"
mkdir "%BDIR%"
pushd "%BDIR%"
cmake -G "MinGW Makefiles" ^
      "-DCMAKE_MAKE_PROGRAM=%MAKE_CMD%" ^
      "-DCMAKE_C_COMPILER=%CC_EXE%" ^
      "-DCMAKE_AR=%AR_EXE%" ^
      "-DCMAKE_RANLIB=%RANLIB_EXE%" ^
      %NL_OPTS% "%SRC_DIR%"
if errorlevel 1 (
    echo [ERROR] CMake configure failed for %PACK_ARCH%
    popd
    exit /b 1
)
cmake --build . --parallel %NUMBER_OF_PROCESSORS%
if errorlevel 1 (
    echo [ERROR] Build failed for %PACK_ARCH%
    popd
    exit /b 1
)
popd
echo [OK] Windows %PACK_ARCH% built
exit /b 0

REM ============================================================
REM :package  <arch>
REM ============================================================
:package
set "PACK_ARCH=%~1"
set "BDIR=%SRC_DIR%\build_mingw_%PACK_ARCH%"
set "PKG=%BDIR%\pkg_out"
echo [Pack] %PACK_ARCH% ...
if exist "%PKG%" rmdir /s /q "%PKG%"
mkdir "%PKG%"
copy /Y "%BDIR%\bin\*.dll" "%PKG%\" >nul 2>nul
copy /Y "%BDIR%\lib\*.a" "%PKG%\" >nul 2>nul
copy /Y "%BDIR%\lib\*.dll.a" "%PKG%\" >nul 2>nul
copy /Y "%SRC_DIR%\include\*.h" "%PKG%\" >nul 2>nul
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%PKG%\*' -DestinationPath '%SRC_DIR%\releases\NetLeaf-%VERSION%-windows-%PACK_ARCH%-mingw.zip' -Force"
rmdir /s /q "%PKG%"
echo [OK] %SRC_DIR%\releases\NetLeaf-%VERSION%-windows-%PACK_ARCH%-mingw.zip
exit /b 0
