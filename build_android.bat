@echo off
REM ============================================================
REM NetLeaf - Android cross-compile build (Windows host, NDK clang)
REM This script ONLY builds Android targets. It does not build
REM Windows / Linux / macOS and never invokes another script.
REM
REM Usage: build_android.bat [arm64-v8a|armeabi-v7a|x86_64|all]  (default: all)
REM        aarch64 is an alias of arm64-v8a
REM
REM NDK lookup order:
REM   ANDROID_NDK / ANDROID_NDK_HOME / ANDROID_NDK_ROOT / NDK_HOME / NDK_ROOT
REM   <ANDROID_HOME>/ndk  <ANDROID_SDK_ROOT>/ndk  common install dirs
REM   (if the directory contains versioned sub-dirs, the highest version wins)
REM
REM Output: build-android\<abi>\stage-lib\  (libnetleaf*.so + public headers)
REM ============================================================

setlocal enabledelayedexpansion
set "VERSION=3.0.0"
set "MIN_SDK=21"
set "SRC_DIR=%~dp0"
set "SRC_DIR=%SRC_DIR:~0,-1%"

REM ---------- parse argument ----------
set "ARCH=%~1"
if not defined ARCH set "ARCH=all"
if /i "%ARCH%"=="aarch64" set "ARCH=arm64-v8a"

set "B_A64=0"
set "B_V7A=0"
set "B_X64=0"
if /i "%ARCH%"=="all"        set "B_A64=1"
if /i "%ARCH%"=="all"        set "B_V7A=1"
if /i "%ARCH%"=="all"        set "B_X64=1"
if /i "%ARCH%"=="arm64-v8a"  set "B_A64=1"
if /i "%ARCH%"=="armeabi-v7a" set "B_V7A=1"
if /i "%ARCH%"=="x86_64"     set "B_X64=1"
if "%B_A64%%B_V7A%%B_X64%"=="000" (
    echo [ERROR] Unknown ABI: %ARCH%
    echo Usage: build_android.bat [arm64-v8a^|armeabi-v7a^|x86_64^|all]
    exit /b 1
)

REM ---------- locate NDK ----------
set "NDK_ROOT="
for %%V in (ANDROID_NDK ANDROID_NDK_HOME ANDROID_NDK_ROOT NDK_HOME NDK_ROOT) do (
    if not defined NDK_ROOT if defined %%V (
        if exist "!%%V!" set "NDK_ROOT=!%%V!"
    )
)
if not defined NDK_ROOT if defined ANDROID_HOME if exist "%ANDROID_HOME%\ndk" set "NDK_ROOT=%ANDROID_HOME%\ndk"
if not defined NDK_ROOT if defined ANDROID_SDK_ROOT if exist "%ANDROID_SDK_ROOT%\ndk" set "NDK_ROOT=%ANDROID_SDK_ROOT%\ndk"
REM environment-derived default locations only (no hardcoded drive letters)
for %%D in (
    "%LOCALAPPDATA%\Android\Sdk\ndk"
    "%USERPROFILE%\AppData\Local\Android\Sdk\ndk"
    "%ProgramFiles%\Android\Sdk\ndk"
) do (
    if not defined NDK_ROOT if exist "%%~D" set "NDK_ROOT=%%~D"
)
if not defined NDK_ROOT (
    echo [ERROR] Android NDK not found.
    echo         Set ANDROID_NDK_HOME to the NDK directory, for example:
    echo           set ANDROID_NDK_HOME=^<path\to\ndk^>
    exit /b 1
)

REM If it is a parent folder with versioned sub-dirs, pick the highest version
REM (versioned NDK dir names sort lexicographically, so the last match wins).
if not exist "!NDK_ROOT!\source.properties" (
    set "PICKED="
    for /d %%V in ("!NDK_ROOT!\*") do (
        if exist "%%~V\source.properties" set "PICKED=%%~V"
    )
    if defined PICKED set "NDK_ROOT=!PICKED!"
)
if not exist "!NDK_ROOT!\source.properties" (
    echo [ERROR] !NDK_ROOT! is not a valid NDK ^(missing source.properties^).
    exit /b 1
)
set "TOOLCHAIN=!NDK_ROOT!\build\cmake\android.toolchain.cmake"
if not exist "!TOOLCHAIN!" (
    echo [ERROR] android.toolchain.cmake missing at !TOOLCHAIN! ^(NDK r21+ required^).
    exit /b 1
)

REM ---------- locate a make program / generator ----------
set "GEN="
set "MAKE_PROG="
if defined LLVM_MINGW if exist "%LLVM_MINGW%\mingw32-make.exe" set "MAKE_PROG=%LLVM_MINGW%\mingw32-make.exe"
if not defined MAKE_PROG if defined CMAKE_MAKE_PROGRAM if exist "%CMAKE_MAKE_PROGRAM%" set "MAKE_PROG=%CMAKE_MAKE_PROGRAM%"
if not defined MAKE_PROG for /f "delims=" %%I in ('where mingw32-make.exe 2^>nul') do if not defined MAKE_PROG set "MAKE_PROG=%%I"
if not defined MAKE_PROG for /f "delims=" %%I in ('where make.exe 2^>nul') do if not defined MAKE_PROG set "MAKE_PROG=%%I"
for /f "delims=" %%I in ('where ninja.exe 2^>nul') do if not defined GEN set "GEN=Ninja"
if not defined GEN if defined MAKE_PROG set "GEN=MinGW Makefiles"
if not defined GEN (
    echo [ERROR] No build tool found ^(ninja / mingw32-make / make^).
    echo         Install ninja, or add LLVM-MinGW bin ^(ships mingw32-make^) to PATH.
    exit /b 1
)

echo ========================================
echo   NetLeaf v%VERSION% - Android build
echo ========================================
echo Source   : %SRC_DIR%
echo NDK      : %NDK_ROOT%
echo Generator: %GEN%
echo Make     : %MAKE_PROG%
echo ABI      : %ARCH%
echo Output   : build-android\^<abi^>\stage-lib\
echo.

set "NL_OPTS=-DCMAKE_BUILD_TYPE=Release -DANDROID_PLATFORM=android-%MIN_SDK% -DANDROID_ARM_NEON=ON -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TLS=OFF -DBUILD_TLS3=ON -DBUILD_MQTT=OFF -DBUILD_MQTT_SERVER=OFF -DBUILD_LANG=ON"

set "FAILED="
if "%B_A64%"=="1" call :build_abi arm64-v8a
if "%B_V7A%"=="1" call :build_abi armeabi-v7a
if "%B_X64%"=="1" call :build_abi x86_64

echo ========================================
echo   Android build finished
echo ========================================
if defined FAILED (
    echo [FAILED ABIs] %FAILED%
    endlocal
    exit /b 1
)
echo Outputs under: %SRC_DIR%\build-android\
endlocal
exit /b 0

REM ============================================================
REM :build_abi  <abi>
REM ============================================================
:build_abi
set "ABI=%~1"
set "BDIR=%SRC_DIR%\build-android\%ABI%"
echo.
echo ----------------------------------------
echo [Build] Android %ABI%
echo ----------------------------------------
if exist "%BDIR%" rmdir /s /q "%BDIR%"
mkdir "%BDIR%"

set "GCMD="
if /i "%GEN%"=="Ninja" set "GCMD=-DCMAKE_MAKE_PROGRAM=ninja.exe"
if /i "%GEN%"=="MinGW Makefiles" set "GCMD=-DCMAKE_MAKE_PROGRAM=%MAKE_PROG%"

cmake -S "%SRC_DIR%" -B "%BDIR%" -G "%GEN%" %GCMD% ^
      -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" ^
      -DANDROID_ABI=%ABI% ^
      %NL_OPTS% ^
      -DCMAKE_C_FLAGS="-fPIE -fdata-sections -ffunction-sections" ^
      -DCMAKE_SHARED_LINKER_FLAGS="-fdata-sections -ffunction-sections -Wl,--gc-sections"
if errorlevel 1 (
    echo [FAIL] configure failed: %ABI%
    set "FAILED=%FAILED% %ABI%"
    exit /b 1
)
cmake --build "%BDIR%" --parallel %NUMBER_OF_PROCESSORS% --target netleaf_core
if errorlevel 1 (
    echo [FAIL] build failed: %ABI%
    set "FAILED=%FAILED% %ABI%"
    exit /b 1
)

set "STAGE=%BDIR%\stage-lib"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
copy /Y "%BDIR%\lib\*netleaf*.so" "%STAGE%\" >nul 2>nul
copy /Y "%SRC_DIR%\include\*.h" "%STAGE%\" >nul 2>nul
echo [OK] %ABI% -^> %STAGE%
exit /b 0
