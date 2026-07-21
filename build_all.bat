@echo off
chcp 65001 >nul 2>&1
REM NetLeaf Multi-Architecture Build Script
REM This script builds NetLeaf for Windows x86, x64, and ARM64

set VERSION=2.2.2

echo ========================================
echo   NetLeaf v%VERSION% Build Script
echo ========================================
echo.
echo Usage: Just run build_all.bat
echo Output: releases/ directory
echo.

REM Create output directories
if not exist "build" mkdir build
if not exist "releases" mkdir releases
if not exist "extensions" mkdir extensions

REM Check if VS environment is available
where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo [错误] 未找到Visual Studio构建工具
    echo 请在Visual Studio Developer Command Prompt中运行此脚本
    echo.
    exit /b 1
)

REM Build for x64
echo [1/3] Building for Windows x64...
if exist "build\x64" rmdir /s /q "build\x64"
mkdir "build\x64"
cd "build\x64"
cmake -G "Visual Studio 17 2022" -A x64 -DBUILD_EXAMPLES=ON -DBUILD_AUTOCOMPLETE=ON -DBUILD_AUTOROUTE=ON -DBUILD_ERRORPAGE=ON -DBUILD_IPC=ON -DBUILD_LINKAGG=ON ..\..
if %errorlevel% neq 0 (
    echo CMake configuration failed for x64!
    cd ..\..
    exit /b 1
)
cmake --build . --config Release
if %errorlevel% neq 0 (
    echo Build failed for x64!
    cd ..\..
    exit /b 1
)
cd ..\..
echo x64 build completed!

REM Create x64 package with example
echo Creating x64 package...
powershell.exe -Command "Compress-Archive -Path 'build\x64\lib\Release\netleaf.lib','build\x64\bin\Release\netleaf.dll','build\x64\bin\Release\example_all_features.exe','build\x64\lib\Release\netleaf_autocomplete.lib','build\x64\bin\Release\netleaf_autocomplete.dll','build\x64\lib\Release\netleaf_autoroute.lib','build\x64\bin\Release\netleaf_autoroute.dll','build\x64\lib\Release\netleaf_errorpage.lib','build\x64\bin\Release\netleaf_errorpage.dll','build\x64\lib\Release\netleaf_ipc.lib','build\x64\bin\Release\netleaf_ipc.dll','build\x64\lib\Release\netleaf_linkagg.lib','build\x64\bin\Release\netleaf_linkagg.dll','include\netleaf.h','include\netleaf_autocomplete.h','include\netleaf_autoroute.h','include\netleaf_errorpage.h','include\netleaf_ipc.h','include\netleaf_linkagg.h','examples\example_all_features.c' -DestinationPath 'releases\NetLeaf-%VERSION%-windows-x64.zip' -Force"
echo x64 package created!
echo.

REM Build for x86 (Win32)
echo [2/3] Building for Windows x86 (Win32)...
if exist "build\x86" rmdir /s /q "build\x86"
mkdir "build\x86"
cd "build\x86"
cmake -G "Visual Studio 17 2022" -A Win32 -DBUILD_EXAMPLES=ON -DBUILD_AUTOCOMPLETE=ON -DBUILD_AUTOROUTE=ON -DBUILD_ERRORPAGE=ON -DBUILD_IPC=ON -DBUILD_LINKAGG=ON ..\..
if %errorlevel% neq 0 (
    echo CMake configuration failed for x86!
    cd ..\..
    exit /b 1
)
cmake --build . --config Release
if %errorlevel% neq 0 (
    echo Build failed for x86!
    cd ..\..
    exit /b 1
)
cd ..\..
echo x86 build completed!

REM Create x86 package with example
echo Creating x86 package...
powershell.exe -Command "Compress-Archive -Path 'build\x86\lib\Release\netleaf.lib','build\x86\bin\Release\netleaf.dll','build\x86\bin\Release\example_all_features.exe','build\x86\lib\Release\netleaf_autocomplete.lib','build\x86\bin\Release\netleaf_autocomplete.dll','build\x86\lib\Release\netleaf_autoroute.lib','build\x86\bin\Release\netleaf_autoroute.dll','build\x86\lib\Release\netleaf_errorpage.lib','build\x86\bin\Release\netleaf_errorpage.dll','include\netleaf.h','examples\example_all_features.c' -DestinationPath 'releases\NetLeaf-%VERSION%-windows-x86.zip' -Force"
echo x86 package created!
echo.

REM Build for ARM64
echo [3/3] Building for Windows ARM64...
if exist "build\arm64" rmdir /s /q "build\arm64"
mkdir "build\arm64"
cd "build\arm64"
cmake -G "Visual Studio 17 2022" -A ARM64 -DBUILD_EXAMPLES=ON -DBUILD_AUTOCOMPLETE=ON -DBUILD_AUTOROUTE=ON -DBUILD_ERRORPAGE=ON -DBUILD_IPC=ON -DBUILD_LINKAGG=ON ..\..
if %errorlevel% neq 0 (
    echo CMake configuration failed for ARM64!
    cd ..\..
    exit /b 1
)
cmake --build . --config Release
if %errorlevel% neq 0 (
    echo Build failed for ARM64!
    cd ..\..
    exit /b 1
)
cd ..\..
echo ARM64 build completed!

REM Create ARM64 package with example
echo Creating ARM64 package...
powershell.exe -Command "Compress-Archive -Path 'build\arm64\lib\Release\netleaf.lib','build\arm64\bin\Release\netleaf.dll','build\arm64\bin\Release\example_all_features.exe','build\arm64\lib\Release\netleaf_autocomplete.lib','build\arm64\bin\Release\netleaf_autocomplete.dll','build\arm64\lib\Release\netleaf_autoroute.lib','build\arm64\bin\Release\netleaf_autoroute.dll','build\arm64\lib\Release\netleaf_errorpage.lib','build\arm64\bin\Release\netleaf_errorpage.dll','build\arm64\lib\Release\netleaf_ipc.lib','build\arm64\bin\Release\netleaf_ipc.dll','build\arm64\lib\Release\netleaf_linkagg.lib','build\arm64\bin\Release\netleaf_linkagg.dll','include\netleaf.h','include\netleaf_autocomplete.h','include\netleaf_autoroute.h','include\netleaf_errorpage.h','include\netleaf_ipc.h','include\netleaf_linkagg.h','examples\example_all_features.c' -DestinationPath 'releases\NetLeaf-%VERSION%-windows-arm64.zip' -Force"
echo ARM64 package created!
echo.

echo ========================================
echo   All builds completed successfully!
echo ========================================
echo.
echo Packages location: releases\
echo   - NetLeaf-%VERSION%-windows-x64.zip
echo   - NetLeaf-%VERSION%-windows-x86.zip
echo   - NetLeaf-%VERSION%-windows-arm64.zip
echo.