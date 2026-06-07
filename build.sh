#!/bin/bash

# ================================================
# NetLeaf v2.1.0 - Linux 统一构建脚本
# ================================================

# Set UTF-8 encoding
export LC_ALL=C.UTF-8
export LANG=C.UTF-8

echo "================================================"
echo "  NetLeaf v2.1.0 Build"
echo "================================================"
echo ""

# 设置架构 (默认x64)
ARCH=$1
if [ -z "$ARCH" ]; then
    ARCH=x64
fi

if [ "$ARCH" = "x64" ]; then
    BUILD_DIR=build
    CMAKE_FLAGS=""
elif [ "$ARCH" = "x86" ]; then
    BUILD_DIR=build_x86
    CMAKE_FLAGS="-DCMAKE_C_FLAGS=-m32"
elif [ "$ARCH" = "arm" ]; then
    BUILD_DIR=build_arm
    CMAKE_FLAGS="-DCMAKE_C_FLAGS=-march=armv7-a"
elif [ "$ARCH" = "arm64" ]; then
    BUILD_DIR=build_arm64
    CMAKE_FLAGS="-DCMAKE_C_FLAGS=-march=armv8-a"
else
    echo "用法: ./build.sh [x64|x86|arm|arm64]"
    echo "默认: x64"
    exit 1
fi

echo "构建架构: $ARCH"
echo "输出目录: $BUILD_DIR"
echo ""

# 清理旧构建
if [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# CMake配置
cmake .. $CMAKE_FLAGS -DWIDE_LIB=ON -DCMAKE_BUILD_TYPE=Release
if [ $? -ne 0 ]; then
    echo ""
    echo "[错误] CMake配置失败"
    cd ..
    exit 1
fi

# 编译
echo ""
cmake --build . --config Release
if [ $? -ne 0 ]; then
    echo ""
    echo "[错误] 编译失败"
    cd ..
    exit 1
fi

cd ..
echo ""
echo "================================================"
echo "  构建成功!"
echo "================================================"
echo ""
echo "库文件:   $BUILD_DIR/lib/"
echo "可执行文件: $BUILD_DIR/bin/"
echo ""
echo "使用示例:"
echo "  #include \"netleaf.h\""
echo "  nl_serve_files(\"./public\", 8080);"
echo ""
