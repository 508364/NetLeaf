@echo off
chcp 65001 >nul 2>&1
REM NetLeaf Android Cross-Compile Script (Windows, NDK based)
REM Pure C core + extensions compiled with NDK clang for arm64-v8a / armeabi-v7a / x86_64
REM
REM Usage: build_android.bat [aarch64^|armeabi-v7a^|x86_64^|all]
REM   aarch64 maps to arm64-v8a (recommended for 64-bit ARM), x86_64 for emulators.
REM   Default: all
REM
REM Requires: Android NDK on PATH or in default locations.

setlocal enabledelayedexpansion
set VERSION=2.4.2
set SRC_DIR=%~dp0
set SRC_DIR=%SRC_DIR:~0,-1%

REM ---------------------------------------------------------------------------
REM 1. Parse architecture argument
REM ---------------------------------------------------------------------------
set "ARCH=%~1"
if not defined ARCH set "ARCH=all"
set "BUILD_A64=0"
set "BUILD_V7A=0"
set "BUILD_X64=0"
if /i "%ARCH%"=="all" (
    set "BUILD_A64=1"
    set "BUILD_V7A=1"
    set "BUILD_X64=1"
)
if /i "%ARCH%"=="aarch64" set "BUILD_A64=1"
if /i "%ARCH%"=="armeabi-v7a" set "BUILD_V7A=1"
if /i "%ARCH%"=="x86_64" set "BUILD_X64=1"
if "%BUILD_A64%"=="0" if "%BUILD_V7A%"=="0" if "%BUILD_X64%"=="0" (
    echo [ERROR] Unknown architecture: %ARCH%
    echo Usage: build_android.bat [aarch64^|armeabi-v7a^|x86_64^|all]
    exit /b 1
)

REM ---------------------------------------------------------------------------
REM 2. Detect Android NDK
REM ---------------------------------------------------------------------------
set "NDK_ROOT="
if defined ANDROID_NDK_HOME set "NDK_ROOT=%ANDROID_NDK_HOME%"
if defined ANDROID_NDK_ROOT set "NDK_ROOT=%ANDROID_NDK_ROOT%"
if not defined NDK_ROOT if exist "C:\Android\Sdk\ndk" set "NDK_ROOT=C:\Android\Sdk\ndk"
if not defined NDK_ROOT if exist "E:\AndroidSdk\ndk" set "NDK_ROOT=E:\AndroidSdk\ndk"

REM If NDK_ROOT is a parent containing versioned sub-directories, pick highest
REM (lexicographic order over versioned dir names: 27.x > 26.x > 25.x).
if defined NDK_ROOT (
    if not exist "!NDK_ROOT!\meta.json" (
        echo Locating NDK under !NDK_ROOT! ...
        for /d %%V in ("!NDK_ROOT!\*") do (
            if exist "%%~V\meta.json" (
                if not defined BEST_NDK (
                    set "BEST_NDK=%%~V"
                ) else (
                    REM Compare: 27.* beats 25.*; simple first-arg test is unreliable
                    REM so we let the user set ANDROID_NDK_HOME to an exact dir if
                    REM multiple versions coexist. Otherwise, take the last match.
                    set "BEST_NDK=%%~V"
                )
            )
        )
        if defined BEST_NDK set "NDK_ROOT=!BEST_NDK!"
    )
)

if not defined NDK_ROOT (
    echo [ERROR] Android NDK not found.
    echo         Install via:  sdkmanager "ndk;27.2.12479018"
    echo         Or set:       ANDROID_NDK_HOME=C:\path\to\ndk
    echo         Default search paths tried:
    echo           C:\Android\Sdk\ndk
    echo           E:\AndroidSdk\ndk
    exit /b 1
)
if not exist "!NDK_ROOT!\meta.json" (
    echo [ERROR] !NDK_ROOT! does not look like a valid NDK (missing meta.json).
    exit /b 1
)
echo Using NDK: !NDK_ROOT!

REM Toolchain file ships with NDK 25+
set "TOOLCHAIN=!NDK_ROOT!\build\cmake\android.toolchain.cmake"
if not exist "!TOOLCHAIN!" (
    echo [ERROR] android.toolchain.cmake not found at !TOOLCHAIN!
    echo         Update NDK to r25 or later.
    exit /b 1
)

REM ---------------------------------------------------------------------------
REM 3. Detect build tool (ninja / nmake / make)
REM ---------------------------------------------------------------------------
set "GEN="
set "BUILD_CMD="
where ninja >nul 2>nul (
    if !errorlevel! equ 0 (
        set "GEN=Ninja"
        set "BUILD_CMD=ninja"
    )
)
if not defined GEN (
    where nmake >nul 2>nul (
        if !errorlevel! equ 0 (
            set "GEN=NMake Makefiles"
            set "BUILD_CMD=nmake"
        )
    )
)
if not defined GEN (
    where make >nul 2>nul (
        if !errorlevel! equ 0 (
            set "GEN=Unix Makefiles"
            set "BUILD_CMD=make"
        )
    )
)
if not defined GEN (
    echo [ERROR] None of ninja / nmake / make found in PATH.
    exit /b 1
)
echo Using generator: !GEN!

REM ---------------------------------------------------------------------------
REM 4. Common CMake flags for Android
REM ---------------------------------------------------------------------------
set "COMMON_OPTS=-DCMAKE_BUILD_TYPE=Release -DANDROID_PLATFORM=android-21 -DANDROID_STL=c++_shared -DANDROID_ARM_NEON=ON"
set "COMMON_OPTS=!COMMON_OPTS! -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF"
set "COMMON_OPTS=!COMMON_OPTS! -DBUILD_TLS=OFF -DBUILD_TLS3=ON -DBUILD_MQTT=OFF -DBUILD_MQTT_SERVER=OFF"
set "COMMON_OPTS=!COMMON_OPTS! -DBUILD_LANG=ON -DBUILD_LINKAGG=ON -DBUILD_VUE=OFF -DBUILD_AUTOCOMPLETE=ON"
set "COMMON_OPTS=!COMMON_OPTS! -DBUILD_AUTOROUTE=OFF -DBUILD_ERRORPAGE=ON -DBUILD_IPC=ON -DBUILD_HTTPS=OFF"

echo ========================================
echo   NetLeaf v!VERSION! Android Build (NDK)
echo ========================================
echo Source: %SRC_DIR%
echo NDK:    !NDK_ROOT!
echo ABI:    %ARCH%
echo.

REM ---------------------------------------------------------------------------
REM 5. Build arm64-v8a (aarch64)
REM ---------------------------------------------------------------------------
if "%BUILD_A64%"=="0" goto :skip_a64
echo ========================================
echo [arm64-v8a] Cross-compiling...
echo ========================================
set "BUILD_DIR=%SRC_DIR%\build-android\arm64-v8a"
if exist "!BUILD_DIR!" rmdir /s /q "!BUILD_DIR!"
mkdir "!BUILD_DIR!"
pushd "!BUILD_DIR!"
cmake -G "!GEN!" -S "%SRC_DIR%" -B . -DCMAKE_TOOLCHAIN_FILE="!TOOLCHAIN!" -DANDROID_ABI=arm64-v8a !COMMON_OPTS!
if !errorlevel! neq 0 (
    echo [ERROR] CMake configure failed for arm64-v8a!
    popd
    exit /b 1
)
if /i "!BUILD_CMD!"=="ninja" (ninja netleaf_core) else if /i "!BUILD_CMD!"=="nmake" (nmake netleaf_core) else (make netleaf_core)
if !errorlevel! neq 0 (
    echo [ERROR] Build failed for arm64-v8a!
    popd
    exit /b 1
)
popd
echo arm64-v8a build completed!

set "STAGE=%SRC_DIR%\build-android\arm64-v8a\stage-lib"
if not exist "!STAGE!" mkdir "!STAGE!"
copy /Y "!BUILD_DIR!\lib\libnetleaf*.so" "!STAGE!\" >nul
copy /Y "%SRC_DIR%\include\*.h" "!STAGE!\" >nul
copy /Y "%SRC_DIR%\CMakeLists.txt" "!STAGE!\" >nul
echo Staged: !STAGE!
echo.

:skip_a64

REM ---------------------------------------------------------------------------
REM 6. Build armeabi-v7a
REM ---------------------------------------------------------------------------
if "%BUILD_V7A%"=="0" goto :skip_v7a
echo ========================================
echo [armeabi-v7a] Cross-compiling...
echo ========================================
set "BUILD_DIR=%SRC_DIR%\build-android\armeabi-v7a"
if exist "!BUILD_DIR!" rmdir /s /q "!BUILD_DIR!"
mkdir "!BUILD_DIR!"
pushd "!BUILD_DIR!"
cmake -G "!GEN!" -S "%SRC_DIR%" -B . -DCMAKE_TOOLCHAIN_FILE="!TOOLCHAIN!" -DANDROID_ABI=armeabi-v7a !COMMON_OPTS!
if !errorlevel! neq 0 (
    echo [ERROR] CMake configure failed for armeabi-v7a!
    popd
    exit /b 1
)
if /i "!BUILD_CMD!"=="ninja" (ninja netleaf_core) else if /i "!BUILD_CMD!"=="nmake" (nmake netleaf_core) else (make netleaf_core)
if !errorlevel! neq 0 (
    echo [ERROR] Build failed for armeabi-v7a!
    popd
    exit /b 1
)
popd
echo armeabi-v7a build completed!

set "STAGE=%SRC_DIR%\build-android\armeabi-v7a\stage-lib"
if not exist "!STAGE!" mkdir "!STAGE!"
copy /Y "!BUILD_DIR!\lib\libnetleaf*.so" "!STAGE!\" >nul
copy /Y "%SRC_DIR%\include\*.h" "!STAGE!\" >nul
copy /Y "%SRC_DIR%\CMakeLists.txt" "!STAGE!\" >nul
echo Staged: !STAGE!
echo.

:skip_v7a

REM ---------------------------------------------------------------------------
REM 7. Build x86_64 (emulator)
REM ---------------------------------------------------------------------------
if "%BUILD_X64%"=="0" goto :skip_x64
echo ========================================
echo [x86_64] Cross-compiling...
echo ========================================
set "BUILD_DIR=%SRC_DIR%\build-android\x86_64"
if exist "!BUILD_DIR!" rmdir /s /q "!BUILD_DIR!"
mkdir "!BUILD_DIR!"
pushd "!BUILD_DIR!"
cmake -G "!GEN!" -S "%SRC_DIR%" -B . -DCMAKE_TOOLCHAIN_FILE="!TOOLCHAIN!" -DANDROID_ABI=x86_64 !COMMON_OPTS!
if !errorlevel! neq 0 (
    echo [ERROR] CMake configure failed for x86_64!
    popd
    exit /b 1
)
if /i "!BUILD_CMD!"=="ninja" (ninja netleaf_core) else if /i "!BUILD_CMD!"=="nmake" (nmake netleaf_core) else (make netleaf_core)
if !errorlevel! neq 0 (
    echo [ERROR] Build failed for x86_64!
    popd
    exit /b 1
)
popd
echo x86_64 build completed!

set "STAGE=%SRC_DIR%\build-android\x86_64\stage-lib"
if not exist "!STAGE!" mkdir "!STAGE!"
copy /Y "!BUILD_DIR!\lib\libnetleaf*.so" "!STAGE!\" >nul
copy /Y "%SRC_DIR%\include\*.h" "!STAGE!\" >nul
copy /Y "%SRC_DIR%\CMakeLists.txt" "!STAGE!\" >nul
echo Staged: !STAGE!
echo.

:skip_x64

REM ---------------------------------------------------------------------------
REM 8. Summary
REM ---------------------------------------------------------------------------
echo ========================================
echo   NetLeaf Android build completed!
echo ========================================
echo.
echo Outputs:
if "%BUILD_A64%"=="1" echo   build-android\arm64-v8a\stage-lib\
if "%BUILD_V7A%"=="1" echo   build-android\armeabi-v7a\stage-lib\
if "%BUILD_X64%"=="1" echo   build-android\x86_64\stage-lib\
echo.
echo Each stage-lib contains:
echo   libnetleaf*.so   (shared library)
echo   *.h              (public headers)
echo   CMakeLists.txt   (for integration)
echo.
endlocal
