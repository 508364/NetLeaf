@echo off
chcp 65001 >nul 2>&1
REM NetLeaf Multi-Architecture Build Script (MSVC)
REM Builds for Windows x64, x86, and ARM64 using Visual Studio compiler
REM This is a pure C project - no C++ compiler needed
REM
REM Usage: Run from Visual Studio Developer Command Prompt
REM        or use "x64 Native Tools Command Prompt for VS 2022"
REM        build_all-MSVC.bat [x64^|x86^|arm64^|all]
REM   amd64 is an alias of x64. Default: all

set VERSION=2.4.1
set SRC_DIR=%~dp0
REM Remove trailing backslash
set SRC_DIR=%SRC_DIR:~0,-1%

REM ?????????(???? if/goto, ???????): x64 / x86 / arm64 / all
set "ARCH=%~1"
if not defined ARCH set "ARCH=all"
if /i "%ARCH%"=="amd64" set "ARCH=x64"
set BUILD_X64=0
set BUILD_X86=0
set BUILD_ARM64=0
if /i "%ARCH%"=="all" set BUILD_X64=1
if /i "%ARCH%"=="all" set BUILD_X86=1
if /i "%ARCH%"=="all" set BUILD_ARM64=1
if /i "%ARCH%"=="x64" set BUILD_X64=1
if /i "%ARCH%"=="x86" set BUILD_X86=1
if /i "%ARCH%"=="arm64" set BUILD_ARM64=1
if /i "%ARCH%"=="all" goto :arch_ok
if /i "%ARCH%"=="x64" goto :arch_ok
if /i "%ARCH%"=="x86" goto :arch_ok
if /i "%ARCH%"=="arm64" goto :arch_ok
echo [ERROR] Unknown architecture: %ARCH%
echo Usage: build_all-MSVC.bat [x64^|x86^|arm64^|all]
echo   amd64 is an alias of x64. Default: all
exit /b 1

:arch_ok
echo ========================================
echo   NetLeaf v%VERSION% Build Script (MSVC - Pure C)
echo ========================================
echo.
echo Usage: Run from Visual Studio Developer Command Prompt
echo Source directory: %SRC_DIR%
echo Output: releases/ directory
echo Target architecture: %ARCH%
echo.

REM Check if VS environment is available
where cl >nul 2>nul
if %errorlevel% equ 0 goto :cl_ok
echo [ERROR] Visual Studio compiler (cl.exe) not found
echo.
echo Please run this script from Visual Studio Developer Command Prompt
echo or use "x64 Native Tools Command Prompt for VS 2022"
echo.
exit /b 1

:cl_ok

REM Create output directories
if not exist "build_x64" mkdir build_x64
if not exist "build_x86" mkdir build_x86
if not exist "build_arm64" mkdir build_arm64
if not exist "releases" mkdir releases

REM Common CMake options (Pure C project)
set COMMON_OPTS=-DBUILD_EXAMPLES=ON -DBUILD_TESTS=ON -DBUILD_TLS=ON -DBUILD_MQTT=ON -DBUILD_MQTT_SERVER=ON

REM ---------------------------------------------------------------------------
if "%BUILD_X64%"=="0" goto :skip_x64
REM 1. Build for x64
echo [1/3] Building for Windows x64...
if exist "build_x64" rmdir /s /q "build_x64"
mkdir "build_x64"
cmake -G "Visual Studio 17 2022" -A x64 %COMMON_OPTS% "%SRC_DIR%"
if %errorlevel% neq 0 ( echo CMake failed for x64! & exit /b 1 )
cd build_x64
cmake --build . --config Release
if %errorlevel% neq 0 ( echo Build failed for x64! & exit /b 1 )
cd ..
echo x64 build completed!

REM Package x64
echo Creating x64 package...
powershell.exe -Command "Compress-Archive -Path 'build_x64\bin\Release\*.dll','include\netleaf*.h' -DestinationPath 'releases\NetLeaf-%VERSION%-windows-x64.zip' -Force"
echo x64 package created!
echo.

:skip_x64
REM ---------------------------------------------------------------------------
if "%BUILD_X86%"=="0" goto :skip_x86
REM 2. Build for x86 (Win32)
echo [2/3] Building for Windows x86...
if exist "build_x86" rmdir /s /q "build_x86"
mkdir "build_x86"
cmake -G "Visual Studio 17 2022" -A Win32 %COMMON_OPTS% "%SRC_DIR%"
if %errorlevel% neq 0 ( echo CMake failed for x86! & exit /b 1 )
cd build_x86
cmake --build . --config Release
if %errorlevel% neq 0 ( echo Build failed for x86! & exit /b 1 )
cd ..
echo x86 build completed!

REM Package x86
echo Creating x86 package...
powershell.exe -Command "Compress-Archive -Path 'build_x86\bin\Release\*.dll','include\netleaf*.h' -DestinationPath 'releases\NetLeaf-%VERSION%-windows-x86.zip' -Force"
echo x86 package created!
echo.

:skip_x86
REM ---------------------------------------------------------------------------
if "%BUILD_ARM64%"=="0" goto :skip_arm64
REM 3. Build for ARM64
echo [3/3] Building for Windows ARM64...
if exist "build_arm64" rmdir /s /q "build_arm64"
mkdir "build_arm64"
cmake -G "Visual Studio 17 2022" -A ARM64 %COMMON_OPTS% "%SRC_DIR%"
if %errorlevel% neq 0 ( echo CMake failed for ARM64! & exit /b 1 )
cd build_arm64
cmake --build . --config Release
if %errorlevel% neq 0 ( echo Build failed for ARM64! & exit /b 1 )
cd ..
echo ARM64 build completed!

REM Package ARM64
echo Creating ARM64 package...
powershell.exe -Command "Compress-Archive -Path 'build_arm64\bin\Release\*.dll','include\netleaf*.h' -DestinationPath 'releases\NetLeaf-%VERSION%-windows-arm64.zip' -Force"
echo ARM64 package created!
echo.

:skip_arm64
REM ---------------------------------------------------------------------------
echo ========================================
echo   All MSVC builds completed successfully!
echo ========================================
echo.
echo Packages location: releases\
if "%BUILD_X64%"=="1" echo   - NetLeaf-%VERSION%-windows-x64.zip
if "%BUILD_X86%"=="1" echo   - NetLeaf-%VERSION%-windows-x86.zip
if "%BUILD_ARM64%"=="1" echo   - NetLeaf-%VERSION%-windows-arm64.zip
echo.
echo Note: For cross-compilation without MSVC, use build_all-Clang.bat
echo
