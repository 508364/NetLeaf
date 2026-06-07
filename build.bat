@echo off
REM ================================================
REM NetLeaf v2.0.0 - Windows 统一构建脚本
REM ================================================

REM 设置控制台编码为UTF-8
chcp 65001 >nul

echo ================================================
echo   NetLeaf v2.0.0 Build
echo ================================================
echo.

REM 设置架构 (默认x64)
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64

if "%ARCH%"=="x64" (
    set CMAKE_ARCH=x64
    set BUILD_DIR=build
) else if "%ARCH%"=="x86" (
    set CMAKE_ARCH=Win32
    set BUILD_DIR=build_x86
) else if "%ARCH%"=="arm64" (
    set CMAKE_ARCH=ARM64
    set BUILD_DIR=build_arm64
) else (
    echo 用法: build.bat [x64^|x86^|arm64]
    echo 默认: x64
    pause
    exit /b 1
)

echo 构建架构: %ARCH%
echo 输出目录: %BUILD_DIR%
echo.

REM 清理旧构建
if exist %BUILD_DIR% rmdir /s /q %BUILD_DIR%
mkdir %BUILD_DIR%
cd %BUILD_DIR%

REM CMake配置
cmake .. -G "Visual Studio 17 2022" -A %CMAKE_ARCH% -DWIDE_LIB=ON
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [错误] CMake配置失败
    cd ..
    pause
    exit /b 1
)

REM 编译
echo.
cmake --build . --config Release
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [错误] 编译失败
    cd ..
    pause
    exit /b 1
)

cd ..
echo.
echo ================================================
echo   构建成功!
echo ================================================
echo.
echo 库文件:   %BUILD_DIR%\lib\
echo 可执行文件: %BUILD_DIR%\bin\
echo.
echo 使用示例:
echo   #include "netleaf.h"
echo   nl_serve_files("./public", 8080);
echo.
pause
