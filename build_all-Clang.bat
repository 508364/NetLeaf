@echo off
chcp 65001 >nul 2>&1
REM NetLeaf Multi-Architecture Build Script (LLVM-MinGW + WSL for Linux/macOS)
REM Pure C project - no C++ compiler needed
REM
REM Usage: build_all-Clang.bat [x64^|x86^|arm64^|all]
REM   amd64 is an alias of x64. Default: all

setlocal enabledelayedexpansion
set VERSION=2.4.1
set SRC_DIR=%~dp0
set SRC_DIR=%SRC_DIR:~0,-1%

REM Parse architecture argument
set "ARCH=%~1"
if not defined ARCH set "ARCH=all"
if /i "%ARCH%"=="amd64" set "ARCH=x64"
set BUILD_X64=0
set BUILD_X86=0
set BUILD_ARM64=0
if /i "%ARCH%"=="all"   set BUILD_X64=1
if /i "%ARCH%"=="all"   set BUILD_X86=1
if /i "%ARCH%"=="all"   set BUILD_ARM64=1
if /i "%ARCH%"=="x64"   set BUILD_X64=1
if /i "%ARCH%"=="x86"   set BUILD_X86=1
if /i "%ARCH%"=="arm64" set BUILD_ARM64=1
if /i "%ARCH%"=="all"   goto :arch_ok
if /i "%ARCH%"=="x64"   goto :arch_ok
if /i "%ARCH%"=="x86"   goto :arch_ok
if /i "%ARCH%"=="arm64" goto :arch_ok
echo [ERROR] Unknown architecture: %ARCH%
echo Usage: build_all-Clang.bat [x64^|x86^|arm64^|all]
exit /b 1

:arch_ok
REM ---------------------------------------------------------------------------
REM 1. Detect LLVM-MinGW (from PATH, not hardcoded)
REM ---------------------------------------------------------------------------
set "LLVM_MINGW="
if defined CC (
    for %%I in ("%CC%") do if exist "%%~dpI" set "LLVM_MINGW=%%~dpI"
)
if not defined LLVM_MINGW for /f "delims=" %%I in ('where x86_64-w64-mingw32-clang.exe 2^>nul') do if exist "%%I" set "LLVM_MINGW=%%~dpI"
if not defined LLVM_MINGW for /f "delims=" %%I in ('where i686-w64-mingw32-clang.exe 2^>nul') do if exist "%%I" set "LLVM_MINGW=%%~dpI"
if not defined LLVM_MINGW for /f "delims=" %%I in ('where aarch64-w64-mingw32-clang.exe 2^>nul') do if exist "%%I" set "LLVM_MINGW=%%~dpI"
if not defined LLVM_MINGW for /f "delims=" %%I in ('where clang.exe 2^>nul') do if exist "%%I" set "LLVM_MINGW=%%~dpI"
if defined LLVM_MINGW if "%LLVM_MINGW:~-1%"=="\" set "LLVM_MINGW=%LLVM_MINGW:~0,-1%"


REM Check required tools
set "CLANG_X64=%LLVM_MINGW%\x86_64-w64-mingw32-clang.exe"
set "CLANG_X86=%LLVM_MINGW%\i686-w64-mingw32-clang.exe"
set "CLANG_ARM=%LLVM_MINGW%\aarch64-w64-mingw32-clang.exe"

if not exist "%CLANG_X64%" (
    echo [ERROR] x86_64-w64-mingw32-clang.exe not found at %LLVM_MINGW%
    exit /b 1
)
if not exist "%CLANG_X86%" (
    echo [WARNING] i686-w64-mingw32-clang.exe not found, x86 build will fail
)
if not exist "%CLANG_ARM%" (
    echo [WARNING] aarch64-w64-mingw32-clang.exe not found, ARM64 build will fail
)

REM Detect make command
set "MAKE_CMD="
where mingw32-make >nul 2>nul && set "MAKE_CMD=mingw32-make"
if not defined MAKE_CMD where make >nul 2>nul && set "MAKE_CMD=make"
if not defined MAKE_CMD (
    echo [ERROR] Neither mingw32-make nor make found in PATH.
    exit /b 1
)
echo Using make command: %MAKE_CMD%

echo ========================================
echo   NetLeaf v%VERSION% Build Script ^(Pure C^)
echo ========================================
echo.
echo Windows builds: LLVM-MinGW ^(Clang/MinGW^)
echo Linux/macOS builds: WSL ^(Ubuntu^)
echo.
echo Source directory: %SRC_DIR%
echo Output: releases/ directory
echo Target architecture: %ARCH%
echo.

if not exist "releases" mkdir releases

set WINDOWS_OPTS=-DBUILD_EXAMPLES=ON -DBUILD_TESTS=ON -DBUILD_TLS=ON -DBUILD_MQTT=ON -DBUILD_MQTT_SERVER=ON

REM ---------------------------------------------------------------------------
REM 2. Windows x64 build
REM ---------------------------------------------------------------------------
if "%BUILD_X64%"=="0" goto :skip_x64
echo ========================================
echo [1/4] Building Windows x64...
echo ========================================
if exist "build_x64" rmdir /s /q "build_x64"
mkdir "build_x64"
pushd "build_x64"
cmake -G "MinGW Makefiles" ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER="%CLANG_X64%" ^
      -DCMAKE_AR="%LLVM_MINGW%\x86_64-w64-mingw32-ar.exe" ^
      -DCMAKE_RANLIB="%LLVM_MINGW%\x86_64-w64-mingw32-ranlib.exe" ^
      %WINDOWS_OPTS% "%SRC_DIR%"
if %errorlevel% neq 0 (
    echo [ERROR] CMake failed for x64!
    popd
    exit /b 1
)
%MAKE_CMD% -j %NUMBER_OF_PROCESSORS%
if %errorlevel% neq 0 (
    echo [ERROR] Build failed for x64!
    popd
    exit /b 1
)
popd
echo x64 build completed!

echo Creating x64 package...
ping -n 2 127.0.0.1 >nul
powershell.exe -NoProfile -Command "Compress-Archive -Path 'build_x64\bin\*.dll','include\netleaf*.h' -DestinationPath 'releases\NetLeaf-%VERSION%-windows-x64.zip' -Force"
echo x64 package created!
echo.

:skip_x64
REM ---------------------------------------------------------------------------
REM 3. Windows x86 build
REM ---------------------------------------------------------------------------
if "%BUILD_X86%"=="0" goto :skip_x86
echo ========================================
echo [2/4] Building Windows x86...
echo ========================================
if exist "build_i686" rmdir /s /q "build_i686"
mkdir "build_i686"
pushd "build_i686"
cmake -G "MinGW Makefiles" ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER="%CLANG_X86%" ^
      -DCMAKE_AR="%LLVM_MINGW%\i686-w64-mingw32-ar.exe" ^
      -DCMAKE_RANLIB="%LLVM_MINGW%\i686-w64-mingw32-ranlib.exe" ^
      %WINDOWS_OPTS% "%SRC_DIR%"
if %errorlevel% neq 0 (
    echo [ERROR] CMake failed for x86!
    popd
    exit /b 1
)
%MAKE_CMD% -j %NUMBER_OF_PROCESSORS%
if %errorlevel% neq 0 (
    echo [ERROR] Build failed for x86!
    popd
    exit /b 1
)
popd
echo x86 build completed!

echo Creating x86 package...
ping -n 2 127.0.0.1 >nul
powershell.exe -NoProfile -Command "Compress-Archive -Path 'build_i686\bin\*.dll','include\netleaf*.h' -DestinationPath 'releases\NetLeaf-%VERSION%-windows-x86.zip' -Force"
echo x86 package created!
echo.

:skip_x86
REM ---------------------------------------------------------------------------
REM 4. Windows ARM64 build
REM ---------------------------------------------------------------------------
if "%BUILD_ARM64%"=="0" goto :skip_arm64
echo ========================================
echo [3/4] Building Windows ARM64...
echo ========================================
if exist "build_arm64" rmdir /s /q "build_arm64"
mkdir "build_arm64"
pushd "build_arm64"
cmake -G "MinGW Makefiles" ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER="%CLANG_ARM%" ^
      -DCMAKE_AR="%LLVM_MINGW%\aarch64-w64-mingw32-ar.exe" ^
      -DCMAKE_RANLIB="%LLVM_MINGW%\aarch64-w64-mingw32-ranlib.exe" ^
      %WINDOWS_OPTS% "%SRC_DIR%"
if %errorlevel% neq 0 (
    echo [ERROR] CMake failed for ARM64!
    popd
    exit /b 1
)
%MAKE_CMD% -j %NUMBER_OF_PROCESSORS%
if %errorlevel% neq 0 (
    echo [ERROR] Build failed for ARM64!
    popd
    exit /b 1
)
popd
echo ARM64 build completed!

echo Creating ARM64 package...
ping -n 2 127.0.0.1 >nul
powershell.exe -NoProfile -Command "Compress-Archive -Path 'build_arm64\bin\*.dll','include\netleaf*.h' -DestinationPath 'releases\NetLeaf-%VERSION%-windows-arm64.zip' -Force"
echo ARM64 package created!
echo.

:skip_arm64
REM ---------------------------------------------------------------------------
echo ========================================
echo   All builds completed!
echo ========================================
echo.
echo Packages location: releases\
if "%BUILD_X64%"=="1"   echo   - NetLeaf-%VERSION%-windows-x64.zip
if "%BUILD_X86%"=="1"   echo   - NetLeaf-%VERSION%-windows-x86.zip
if "%BUILD_ARM64%"=="1" echo   - NetLeaf-%VERSION%-windows-arm64.zip
echo   - Linux/macOS packages ^(generated by WSL^)
echo.
echo Note: For MSVC builds, use build_all-MSVC.bat
echo.

endlocal
