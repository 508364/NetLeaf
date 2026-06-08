#!/bin/bash

set -e

export LC_ALL=C.UTF-8
export LANG=C.UTF-8

echo "================================================"
echo "  NetLeaf v2.1.5 - Linux 一键构建"
echo "================================================"
echo ""

if ! command -v cmake &> /dev/null; then
    echo "[错误] 未安装CMake"
    exit 1
fi

mkdir -p releases

BUILD_SUCCESS=0
TOTAL_BUILDS=0

declare -A ARCH_COMPILERS=(
    ["amd64"]="x86_64-linux-gnu-gcc"
    ["i686"]="i686-linux-gnu-gcc"
    ["arm"]="arm-linux-gnueabihf-gcc"
    ["arm64"]="aarch64-linux-gnu-gcc"
    ["riscv64"]="riscv64-linux-gnu-gcc"
    ["ppc64le"]="powerpc64le-linux-gnu-gcc"
    ["mips"]="mipsel-linux-gnu-gcc"
    ["s390x"]="s390x-linux-gnu-gcc"
)

SUCCESSFUL_ARCHES=()

echo "[阶段1/3: 检测可用的编译器...]"
echo ""

AVAILABLE_ARCHES=()
for arch in "${!ARCH_COMPILERS[@]}"; do
    compiler=${ARCH_COMPILERS[$arch]}
    if command -v "$compiler" &> /dev/null; then
        AVAILABLE_ARCHES+=("$arch")
        echo "  ✅ $arch: $compiler"
    fi
done

if [[ ${#AVAILABLE_ARCHES[@]} -eq 0 ]]; then
    if command -v gcc &> /dev/null; then
        AVAILABLE_ARCHES=("x64")
    else
        echo "[错误] 未找到任何编译器!"
        exit 1
    fi
fi

echo ""
echo "[阶段2/3: 构建所有架构...]"
echo ""

for ARCH in "${AVAILABLE_ARCHES[@]}"; do
    TOTAL_BUILDS=$((TOTAL_BUILDS + 1))
    echo "[$TOTAL_BUILDS/${#AVAILABLE_ARCHES[@]}] 构建 Linux $ARCH..."
    
    BUILD_DIR="build_${ARCH}"
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    export CC=${ARCH_COMPILERS[$ARCH]:-gcc}
    export CXX=${ARCH_COMPILERS[$ARCH]:-g++}
    
    if cmake .. -DCMAKE_BUILD_TYPE=Release; then
        if cmake --build .; then
            echo "[OK] Linux $ARCH 构建成功"
            BUILD_SUCCESS=$((BUILD_SUCCESS + 1))
            SUCCESSFUL_ARCHES+=("$ARCH")
        fi
    fi
    
    cd ..
done

echo ""
echo "[阶段3/3: 打包...]"
echo ""

for ARCH in "${SUCCESSFUL_ARCHES[@]}"; do
    BUILD_DIR="build_${ARCH}"
    echo "  打包 Linux $ARCH..."
    tar -czvf "releases/NetLeaf-2.1.5-linux-${ARCH}.tar.gz" \
        "$BUILD_DIR/lib/libnetleaf.so" \
        "$BUILD_DIR/lib/libnetleaf.a" \
        "include/netleaf.h"
done

echo ""
echo "================================================"
echo "  构建完成"
echo "================================================"
echo "成功构建: $BUILD_SUCCESS / $TOTAL_BUILDS"
echo ""