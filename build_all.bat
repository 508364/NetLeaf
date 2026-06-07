@echo off
REM NetLeaf Multi-Architecture Build Script
REM This script builds NetLeaf for Windows x86, x64, and ARM64

echo ========================================
echo   NetLeaf v1.9.5 Build Script
echo ========================================
echo.

REM Create output directories
if not exist "build" mkdir build

REM Check if VS environment is available
where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: Visual Studio build tools not found in PATH.
    echo Please run this script from a Visual Studio Developer Command Prompt.
    echo.
    pause
    exit /b 1
)

REM Build for x64
echo [1/3] Building for Windows x64...
if exist "build\x64" rmdir /s /q "build\x64"
mkdir "build\x64"
cd "build\x64"
cmake -G "Visual Studio 17 2022" -A x64 ..\..
if %errorlevel% neq 0 (
    echo CMake configuration failed for x64!
    cd ..\..
    pause
    exit /b 1
)
cmake --build . --config Release
if %errorlevel% neq 0 (
    echo Build failed for x64!
    cd ..\..
    pause
    exit /b 1
)
cd ..\..
echo x64 build completed!
echo.

REM Build for x86 (Win32)
echo [2/3] Building for Windows x86 (Win32)...
if exist "build\x86" rmdir /s /q "build\x86"
mkdir "build\x86"
cd "build\x86"
cmake -G "Visual Studio 17 2022" -A Win32 ..\..
if %errorlevel% neq 0 (
    echo CMake configuration failed for x86!
    cd ..\..
    pause
    exit /b 1
)
cmake --build . --config Release
if %errorlevel% neq 0 (
    echo Build failed for x86!
    cd ..\..
    pause
    exit /b 1
)
cd ..\..
echo x86 build completed!
echo.

REM Build for ARM64
echo [3/3] Building for Windows ARM64...
if exist "build\arm64" rmdir /s /q "build\arm64"
mkdir "build\arm64"
cd "build\arm64"
cmake -G "Visual Studio 17 2022" -A ARM64 ..\..
if %errorlevel% neq 0 (
    echo CMake configuration failed for ARM64!
    cd ..\..
    pause
    exit /b 1
)
cmake --build . --config Release
if %errorlevel% neq 0 (
    echo Build failed for ARM64!
    cd ..\..
    pause
    exit /b 1
)
cd ..\..
echo ARM64 build completed!
echo.

echo ========================================
echo   All builds completed successfully!
echo ========================================
echo.
echo Output directories:
echo   x64: build\x64\bin\Windows\x64\Release
echo   x86: build\x86\bin\Windows\x86\Release
echo   ARM64: build\arm64\bin\Windows\arm64\Release
echo.
echo Libraries can be found in:
echo   build\x64\lib\Windows\x64\Release
echo   build\x86\lib\Windows\x86\Release
echo   build\arm64\lib\Windows\arm64\Release
echo.
pause
