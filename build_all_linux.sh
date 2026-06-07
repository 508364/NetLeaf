#!/bin/bash

# ================================================
# NetLeaf v2.1.0 - Linux 全架构构建脚本
# 自动检测可用交叉编译工具链并编译
# ================================================

# Set UTF-8 encoding
export LC_ALL=C.UTF-8
export LANG=C.UTF-8

echo "================================================"
echo "  NetLeaf v2.1.0 - Linux 全架构构建"
echo "================================================"
echo ""

# 检查CMake
if ! command -v cmake &> /dev/null; then
    echo "[错误] 未安装CMake，请先安装: sudo apt install cmake"
    exit 1
fi

BUILD_SUCCESS=0
TOTAL_BUILDS=0

# 定义所有支持的架构和对应的编译器
declare -A ARCH_COMPILERS=(
    ["x64"]="x86_64-linux-gnu-gcc"
    ["x86"]="i686-linux-gnu-gcc"
    ["arm"]="arm-linux-gnueabihf-gcc"
    ["arm64"]="aarch64-linux-gnu-gcc"
    ["riscv64"]="riscv64-linux-gnu-gcc"
    ["ppc64le"]="powerpc64le-linux-gnu-gcc"
    ["mips"]="mipsel-linux-gnu-gcc"
    ["s390x"]="s390x-linux-gnu-gcc"
)

declare -A ARCH_FLAGS=(
    ["x64"]=""
    ["x86"]="-DCMAKE_C_FLAGS=-m32"
    ["arm"]="-DCMAKE_C_FLAGS=-march=armv7-a"
    ["arm64"]="-DCMAKE_C_FLAGS=-march=armv8-a"
    ["riscv64"]=""
    ["ppc64le"]=""
    ["mips"]=""
    ["s390x"]=""
)

declare -A ARCH_CC=(
    ["x64"]="x86_64-linux-gnu-gcc"
    ["x86"]="i686-linux-gnu-gcc"
    ["arm"]="arm-linux-gnueabihf-gcc"
    ["arm64"]="aarch64-linux-gnu-gcc"
    ["riscv64"]="riscv64-linux-gnu-gcc"
    ["ppc64le"]="powerpc64le-linux-gnu-gcc"
    ["mips"]="mipsel-linux-gnu-gcc"
    ["s390x"]="s390x-linux-gnu-gcc"
)

declare -A ARCH_CXX=(
    ["x64"]="x86_64-linux-gnu-g++"
    ["x86"]="i686-linux-gnu-g++"
    ["arm"]="arm-linux-gnueabihf-g++"
    ["arm64"]="aarch64-linux-gnu-g++"
    ["riscv64"]="riscv64-linux-gnu-g++"
    ["ppc64le"]="powerpc64le-linux-gnu-g++"
    ["mips"]="mipsel-linux-gnu-g++"
    ["s390x"]="s390x-linux-gnu-g++"
)

# 检测可用的交叉编译工具链
echo "[检测可用的交叉编译工具链...]"
echo ""

AVAILABLE_ARCHES=()
for arch in "${!ARCH_COMPILERS[@]}"; do
    compiler=${ARCH_COMPILERS[$arch]}
    if command -v "$compiler" &> /dev/null; then
        AVAILABLE_ARCHES+=("$arch")
        echo "  ✅ $arch: $compiler (可用)"
    else
        echo "  ❌ $arch: $compiler (未安装)"
    fi
done

# 如果没有检测到任何交叉编译器，尝试使用本地编译器
if [ ${#AVAILABLE_ARCHES[@]} -eq 0 ]; then
    if command -v gcc &> /dev/null; then
        echo ""
        echo "[警告] 未检测到交叉编译工具链，使用本地gcc"
        AVAILABLE_ARCHES=("x64")
    else
        echo "[错误] 未找到任何编译器!"
        exit 1
    fi
fi

echo ""
echo "[开始构建 ${#AVAILABLE_ARCHES[@]} 个可用架构...]"
echo ""

# 开始构建
for ARCH in "${AVAILABLE_ARCHES[@]}"; do
    TOTAL_BUILDS=$((TOTAL_BUILDS + 1))
    echo "[$TOTAL_BUILDS/${#AVAILABLE_ARCHES[@]}] 构建 Linux $ARCH..."
    
    # 清理旧的构建
    if [ -d "build_$ARCH" ]; then
        rm -rf "build_$ARCH"
    fi
    mkdir -p "build_$ARCH"
    cd "build_$ARCH"
    
    # 设置交叉编译环境变量
    export CC=${ARCH_CC[$ARCH]}
    export CXX=${ARCH_CXX[$ARCH]}
    CMAKE_FLAGS=${ARCH_FLAGS[$ARCH]}
    
    # 配置CMake
    if cmake .. $CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Release; then
        if cmake --build . --config Release; then
            echo "[OK] Linux $ARCH 构建成功"
            BUILD_SUCCESS=$((BUILD_SUCCESS + 1))
            
            # 创建发布包
            mkdir -p ../releases/Linux
            tar -cJf "../releases/Linux/NetLeaf-2.1.0-linux-$ARCH.tar.xz" lib/libnetleaf.a lib/libnetleaf.so.2.1.0 lib/libnetleaf.so.1 lib/libnetleaf.so
            echo "     发布包已创建: releases/Linux/NetLeaf-2.1.0-linux-$ARCH.tar.xz"
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

for ARCH in "${AVAILABLE_ARCHES[@]}"; do
    if [ -f "build_$ARCH/lib/libnetleaf.a" ]; then
        echo "  $ARCH: build_$ARCH/lib/"
    fi
done

echo ""
echo "发布包位置: releases/Linux/"
echo ""
echo "提示: 如需其他架构，请安装对应的交叉编译工具链。"