#!/bin/bash

# ================================================
# NetLeaf v2.0.0 - Linux 全架构构建脚本
# 同时构建 x86, x64, ARM, ARM64
# ================================================

# Set UTF-8 encoding
export LC_ALL=C.UTF-8
export LANG=C.UTF-8

echo "================================================"
echo "  NetLeaf v2.0.0 - Linux 全架构构建"
echo "================================================"
echo ""

# 检查CMake
if ! command -v cmake &> /dev/null; then
    echo "[错误] 未安装CMake，请先安装: sudo apt install cmake"
    exit 1
fi

BUILD_SUCCESS=0
TOTAL_BUILDS=0

# 架构列表
ARCHS=("x86" "x64" "arm" "arm64")

for ARCH in "${ARCHS[@]}"; do
    TOTAL_BUILDS=$((TOTAL_BUILDS + 1))
    echo ""
    echo "[$TOTAL_BUILDS/${#ARCHS[@]}] 构建 Linux $ARCH..."
    
    # 清理旧的构建
    if [ -d "build_$ARCH" ]; then
        rm -rf "build_$ARCH"
    fi
    mkdir -p "build_$ARCH"
    cd "build_$ARCH"
    
    # 根据架构设置编译标志
    case $ARCH in
        x86)
            CMAKE_FLAGS="-DCMAKE_C_FLAGS=-m32"
            ;;
        x64)
            CMAKE_FLAGS=""
            ;;
        arm)
            CMAKE_FLAGS="-DCMAKE_C_FLAGS=-march=armv7-a"
            ;;
        arm64)
            CMAKE_FLAGS="-DCMAKE_C_FLAGS=-march=armv8-a"
            ;;
    esac
    
    # 配置CMake
    if cmake .. $CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Release; then
        if cmake --build . --config Release; then
            echo "[OK] Linux $ARCH 构建成功"
            BUILD_SUCCESS=$((BUILD_SUCCESS + 1))
        else
            echo "[错误] Linux $ARCH 构建失败"
        fi
    else
        echo "[错误] Linux $ARCH CMake 配置失败"
    fi
    
    cd ..
done

# ================================================
# 构建完成报告
# ================================================
echo ""
echo "================================================"
echo "  构建完成"
echo "================================================"
echo "成功构建: $BUILD_SUCCESS / $TOTAL_BUILDS"
echo ""
echo "输出位置:"

for ARCH in "${ARCHS[@]}"; do
    if [ -f "build_$ARCH/lib/Linux/$ARCH/libnetleaf.a" ]; then
        echo "  $ARCH: build_$ARCH/lib/Linux/$ARCH/"
    fi
done

echo ""
echo "提示: 如需要交叉编译，请先安装交叉编译工具链。"
