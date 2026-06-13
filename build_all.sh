#!/bin/bash
# NetLeaf Multi-Architecture Build Script for Linux
# This script builds NetLeaf for multiple Linux architectures
# It automatically detects available cross-compilers and builds packages

VERSION="2.2.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
RELEASES_DIR="${SCRIPT_DIR}/releases"

# Cross-compiler mappings: architecture -> cross-compiler prefix
declare -A CROSS_COMPILERS
CROSS_COMPILERS=(
    ["i686"]="i686-linux-gnu"
    ["amd64"]="x86_64-linux-gnu"
    ["arm"]="arm-linux-gnueabihf"
    ["arm64"]="aarch64-linux-gnu"
    ["mips"]="mips-linux-gnu"
    ["mipsel"]="mipsel-linux-gnu"
    ["mips64"]="mips64-linux-gnuabi64"
    ["mips64el"]="mips64el-linux-gnuabi64"
    ["powerpc"]="powerpc-linux-gnu"
    ["powerpc64"]="powerpc64-linux-gnu"
    ["powerpc64le"]="powerpc64le-linux-gnu"
    ["riscv64"]="riscv64-linux-gnu"
    ["s390x"]="s390x-linux-gnu"
)

# Architecture display names
declare -A ARCH_NAMES
ARCH_NAMES=(
    ["x86"]="x86"
    ["x86_64"]="x86_64"
    ["arm"]="ARM"
    ["arm64"]="ARM64"
    ["riscv64"]="RISC-V 64"
    ["powerpc64le"]="PowerPC 64 LE"
    ["mips64el"]="MIPS64 EL"
    ["s390x"]="s390x"
)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "========================================"
echo "  NetLeaf v${VERSION} Linux Build Script"
echo "========================================"
echo

# Create output directories
mkdir -p "${BUILD_DIR}"
mkdir -p "${RELEASES_DIR}"

# Check if cmake is available
if ! command -v cmake &> /dev/null; then
    echo -e "${RED}[ERROR] cmake not found. Please install cmake first.${NC}"
    exit 1
fi

# Function to check if cross-compiler exists
check_compiler() {
    local compiler="$1"
    if [ -z "$compiler" ]; then
        # Native compiler
        command -v gcc &> /dev/null && command -v ar &> /dev/null
        return $?
    else
        command -v "${compiler}-gcc" &> /dev/null
        return $?
    fi
}

# Function to get compiler prefix
get_compiler_prefix() {
    local arch="$1"
    local compiler="${CROSS_COMPILERS[$arch]}"
    if [ -n "$compiler" ]; then
        echo "$compiler"
    else
        echo ""
    fi
}

# Function to build for a specific architecture
build_arch() {
    local arch="$1"
    local compiler_prefix
    compiler_prefix=$(get_compiler_prefix "$arch")
    local arch_name="${ARCH_NAMES[$arch]}"
    
    echo "----------------------------------------"
    echo -e "[Building] Linux ${arch_name}..."
    
    local arch_build_dir="${BUILD_DIR}/linux_${arch}"
    if [ -d "${arch_build_dir}" ]; then
        rm -rf "${arch_build_dir}"
    fi
    mkdir -p "${arch_build_dir}"
    
    cd "${arch_build_dir}"
    
    # Set up cross-compilation environment
    export CROSS_COMPILE="${compiler_prefix}"
    
    # Build command
    local cmake_args=()
    cmake_args+=("-DCMAKE_BUILD_TYPE=Release")
    cmake_args+=("-DWIDE_LIB=ON")
    cmake_args+=("-DBUILD_SHARED_LIBS=ON")
    cmake_args+=("-DBUILD_STATIC_LIBS=ON")
    cmake_args+=("-DBUILD_EXAMPLES=ON")
    cmake_args+=("-DBUILD_AUTOCOMPLETE=ON")
    cmake_args+=("-DBUILD_AUTOROUTE=ON")
    cmake_args+=("-DBUILD_ERRORPAGE=ON")
    
    if [ -n "$compiler_prefix" ]; then
        # Cross-compilation: set toolchain variables
        local gcc_path
        gcc_path=$(command -v "${compiler_prefix}-gcc")
        local ar_path
        ar_path=$(command -v "${compiler_prefix}-ar")
        local ranlib_path
        
        cmake_args+=("-DCMAKE_C_COMPILER=${gcc_path}")
        cmake_args+=("-DCMAKE_AR=${ar_path}")
        
        # Handle missing ranlib (some cross-compilers don't have it)
        if command -v "${compiler_prefix}-ranlib" &> /dev/null; then
            ranlib_path=$(command -v "${compiler_prefix}-ranlib")
        else
            ranlib_path="${ar_path}"
        fi
        cmake_args+=("-DCMAKE_RANLIB=${ranlib_path}")
        
        cmake_args+=("-DCMAKE_STRIP=$(command -v "${compiler_prefix}-strip")")
        cmake_args+=("-DCMAKE_SYSTEM_NAME=Linux")
    fi
    
    cmake "${cmake_args[@]}" "${SCRIPT_DIR}"
    if [ $? -ne 0 ]; then
        echo -e "${RED}[FAIL] CMake configuration failed for ${arch_name}.${NC}"
        cd "${SCRIPT_DIR}"
        return 1
    fi
    
    cmake --build . -j$(nproc)
    if [ $? -ne 0 ]; then
        echo -e "${RED}[FAIL] Build failed for ${arch_name}.${NC}"
        cd "${SCRIPT_DIR}"
        return 1
    fi
    
    cd "${SCRIPT_DIR}"
    echo -e "${GREEN}[OK] Linux ${arch_name} build completed!${NC}"
    return 0
}

# Function to package for a specific architecture
package_arch() {
    local arch="$1"
    local arch_name="${ARCH_NAMES[$arch]}"
    
    echo "Creating ${arch_name} package..."
    
    local arch_build_dir="${BUILD_DIR}/linux_${arch}"
    cd "${arch_build_dir}"
    
    # Create temporary directory for packaging
    local PKG_TMP="pkg_tmp_${arch}_$$"
    mkdir -p "${PKG_TMP}"
    
    # Copy library files
    for f in lib/*.so lib/*.so.* lib/*.a; do
        if [ -e "${f}" ]; then
            cp -r "${f}" "${PKG_TMP}/" 2>/dev/null || true
        fi
    done
    
    # Copy executable and source files
    if [ -e "bin/example_all_features" ]; then
        cp -r bin/example_all_features "${PKG_TMP}/"
    fi
    if [ -e "../../include/netleaf.h" ]; then
        cp -r ../../include/netleaf.h "${PKG_TMP}/"
    fi
    if [ -e "../../examples/example_all_features.c" ]; then
        cp -r ../../examples/example_all_features.c "${PKG_TMP}/"
    fi
    
    # Create tar.gz package
    tar -czf "${RELEASES_DIR}/NetLeaf-${VERSION}-linux-${arch}.tar.gz" -C "${PKG_TMP}" .
    rm -rf "${PKG_TMP}"
    
    echo -e "${GREEN}[OK] ${arch_name} package created!${NC}"
}

# Discover available architectures and build
echo "Scanning for available compilers..."
echo

BUILD_ARCHS=()

for arch in "${!CROSS_COMPILERS[@]}"; do
    compiler_prefix=$(get_compiler_prefix "$arch")
    arch_name="${ARCH_NAMES[$arch]}"
    
    if check_compiler "$compiler_prefix"; then
        BUILD_ARCHS+=("$arch")
        echo -e "${GREEN}  [FOUND]${NC} Linux ${arch_name} compiler available"
    else
        echo -e "${YELLOW}  [SKIP]${NC} Linux ${arch_name} compiler not found"
    fi
done

echo
echo "Found ${#BUILD_ARCHS[@]} architecture(s) to build:"

if [ ${#BUILD_ARCHS[@]} -eq 0 ]; then
    echo -e "${RED}[ERROR] No compilers found!${NC}"
    echo "Install cross-compilers with:"
    echo "  sudo apt install gcc gcc-multilib"
    echo "  sudo apt install gcc-arm-linux-gnueabihf gcc-aarch64-linux-gnu"
    echo "  sudo apt install gcc-riscv64-linux-gnu"
    echo "  sudo apt install gcc-powerpc64le-linux-gnu"
    echo "  sudo apt install gcc-mips64el-linux-gnu"
    echo "  sudo apt install gcc-s390x-linux-gnu"
    exit 1
fi

# Build and package each architecture
for arch in "${BUILD_ARCHS[@]}"; do
    if build_arch "$arch"; then
        package_arch "$arch"
        echo
    fi
done

echo "========================================"
echo -e "${GREEN}  All builds completed successfully!${NC}"
echo "========================================"
echo
echo "Packages location: ${RELEASES_DIR}/"
for arch in "${BUILD_ARCHS[@]}"; do
    arch_name="${ARCH_NAMES[$arch]}"
    echo "  - NetLeaf-${VERSION}-linux-${arch}.tar.gz"
done
echo
