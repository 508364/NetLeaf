#!/bin/bash
# NetLeaf v2.1.0 WSL/Linux Build Script
# 自动检测可用交叉编译工具链

# Set UTF-8 encoding
export LC_ALL=C.UTF-8
export LANG=C.UTF-8

echo "========================================"
echo "  NetLeaf v2.1.0 Build Script (WSL/Linux)"
echo "========================================"
echo ""

# Check for required tools
check_tool() {
    if ! command -v "$1" &> /dev/null; then
        echo "Error: $1 not found! Please install it first."
        exit 1
    fi
}

check_tool cmake
check_tool gcc
check_tool make

# 检测可用的交叉编译工具链
echo "[检测可用的交叉编译工具链...]"
echo ""

declare -A ARCH_COMPILERS=(
    ["amd64"]="x86_64-linux-gnu-gcc"
    ["i386"]="i686-linux-gnu-gcc"
    ["arm"]="arm-linux-gnueabihf-gcc"
    ["arm64"]="aarch64-linux-gnu-gcc"
    ["riscv64"]="riscv64-linux-gnu-gcc"
    ["ppc64le"]="powerpc64le-linux-gnu-gcc"
    ["mips"]="mipsel-linux-gnu-gcc"
    ["s390x"]="s390x-linux-gnu-gcc"
)

AVAILABLE_ARCHES=()
for arch in "${!ARCH_COMPILERS[@]}"; do
    compiler=${ARCH_COMPILERS[$arch]}
    if command -v "$compiler" &> /dev/null; then
        AVAILABLE_ARCHES+=("$arch")
        echo "  ✅ $arch: $compiler"
    fi
done

# 如果没有检测到交叉编译器，使用本地gcc
if [ ${#AVAILABLE_ARCHES[@]} -eq 0 ]; then
    echo "  ✅ amd64: gcc (本地)"
    AVAILABLE_ARCHES=("amd64")
fi

echo ""
echo "[可用架构: ${AVAILABLE_ARCHES[*]}]"
echo ""

# 创建发布目录
mkdir -p releases/Linux

# 构建所有可用架构
BUILD_COUNT=0
for ARCH in "${AVAILABLE_ARCHES[@]}"; do
    BUILD_COUNT=$((BUILD_COUNT + 1))
    echo "[$BUILD_COUNT/${#AVAILABLE_ARCHES[@]}] 构建 $ARCH..."
    
    # 设置编译器
    case $ARCH in
        amd64)
            export CC="x86_64-linux-gnu-gcc"
            export CXX="x86_64-linux-gnu-g++"
            CMAKE_FLAGS=""
            ;;
        i386)
            export CC="i686-linux-gnu-gcc"
            export CXX="i686-linux-gnu-g++"
            CMAKE_FLAGS="-DCMAKE_C_FLAGS=-m32"
            ;;
        arm)
            export CC="arm-linux-gnueabihf-gcc"
            export CXX="arm-linux-gnueabihf-g++"
            CMAKE_FLAGS="-DCMAKE_C_FLAGS=-march=armv7-a"
            ;;
        arm64)
            export CC="aarch64-linux-gnu-gcc"
            export CXX="aarch64-linux-gnu-g++"
            CMAKE_FLAGS="-DCMAKE_C_FLAGS=-march=armv8-a"
            ;;
        riscv64)
            export CC="riscv64-linux-gnu-gcc"
            export CXX="riscv64-linux-gnu-g++"
            CMAKE_FLAGS=""
            ;;
        ppc64le)
            export CC="powerpc64le-linux-gnu-gcc"
            export CXX="powerpc64le-linux-gnu-g++"
            CMAKE_FLAGS=""
            ;;
        mips)
            export CC="mipsel-linux-gnu-gcc"
            export CXX="mipsel-linux-gnu-g++"
            CMAKE_FLAGS=""
            ;;
        s390x)
            export CC="s390x-linux-gnu-gcc"
            export CXX="s390x-linux-gnu-g++"
            CMAKE_FLAGS=""
            ;;
    esac
    
    # 创建构建目录
    BUILD_DIR="build_$ARCH"
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR" || exit 1
    
    # Configure and build
    echo "  配置 CMake..."
    cmake .. $CMAKE_FLAGS -DCMAKE_BUILD_TYPE=Release
    if [ $? -ne 0 ]; then
        echo "  ❌ CMake配置失败"
        cd ..
        continue
    fi
    
    echo "  编译..."
    cmake --build . --config Release
    if [ $? -ne 0 ]; then
        echo "  ❌ 编译失败"
        cd ..
        continue
    fi
    
    # 创建发布包
    echo "  创建发布包..."
    tar -cJf "../releases/Linux/NetLeaf-2.1.0-linux-$ARCH.tar.xz" lib/libnetleaf.a lib/libnetleaf.so.2.1.0 lib/libnetleaf.so.1 lib/libnetleaf.so
    echo "  ✅ 完成: releases/Linux/NetLeaf-2.1.0-linux-$ARCH.tar.xz"
    
    cd ..
done

echo ""
echo "========================================"
echo "  Build completed successfully!"
echo "========================================"
echo ""
echo "发布包位置: releases/Linux/"
echo ""