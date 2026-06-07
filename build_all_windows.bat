@echo off
REM ================================================
REM NetLeaf v2.0.0 - Windows 全架构构建脚本
REM 同时构建 x86, x64, ARM64
REM ================================================

REM 设置控制台编码为UTF-8
chcp 65001 >nul

echo ================================================
echo   NetLeaf v2.0.0 - Windows 全架构构建
echo ================================================
echo.

REM 检查是否在开发人员命令提示符中
where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo [错误] 未找到编译器！
    echo 请在 "Visual Studio Developer Command Prompt" 中运行此脚本
    pause
    exit /b 1
)

set BUILD_SUCCESS=0

REM ================================================
REM 1. 构建 x64 (64位) - 默认
REM ================================================
echo.
echo [1/3] 构建 Windows x64 ...
if exist build rmdir /s /q build
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DWIDE_LIB=ON
if %errorlevel% equ 0 (
    cmake --build . --config Release
    if %errorlevel% equ 0 (
        echo [OK] x64 构建成功
        set /a BUILD_SUCCESS+=1
    ) else (
        echo [错误] x64 构建失败
    )
) else (
    echo [错误] x64 CMake 配置失败
)
cd ..

REM ================================================
REM 2. 构建 x86 (32位)
REM ================================================
echo.
echo [2/3] 构建 Windows x86 ...
if exist build_x86 rmdir /s /q build_x86
mkdir build_x86
cd build_x86
cmake .. -G "Visual Studio 17 2022" -A Win32 -DWIDE_LIB=ON
if %errorlevel% equ 0 (
    cmake --build . --config Release
    if %errorlevel% equ 0 (
        echo [OK] x86 构建成功
        set /a BUILD_SUCCESS+=1
    ) else (
        echo [错误] x86 构建失败
    )
) else (
    echo [错误] x86 CMake 配置失败
)
cd ..

REM ================================================
REM 3. 构建 ARM64
REM ================================================
echo.
echo [3/3] 构建 Windows ARM64 ...
if exist build_arm64 rmdir /s /q build_arm64
mkdir build_arm64
cd build_arm64
cmake .. -G "Visual Studio 17 2022" -A ARM64 -DWIDE_LIB=ON
if %errorlevel% equ 0 (
    cmake --build . --config Release
    if %errorlevel% equ 0 (
        echo [OK] ARM64 构建成功
        set /a BUILD_SUCCESS+=1
    ) else (
        echo [错误] ARM64 构建失败
    )
) else (
    echo [错误] ARM64 CMake 配置失败
)
cd ..

REM ================================================
REM 构建完成报告
REM ================================================
echo.
echo ================================================
echo   构建完成
echo ================================================
echo 成功构建: %BUILD_SUCCESS% / 3
echo.
echo 输出位置:
if exist build\bin\Release\netleaf.dll (
    echo   x64: build\bin\Release\
)
if exist build_x86\bin\Release\netleaf.dll (
    echo   x86: build_x86\bin\Release\
)
if exist build_arm64\bin\Release\netleaf.dll (
    echo   ARM64: build_arm64\bin\Release\
)
echo.
echo 所有库位于: [架构]\lib\Release\
echo.
pause
