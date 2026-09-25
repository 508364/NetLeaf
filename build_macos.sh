#!/bin/bash
# NetLeaf macOS Build Script (Cross-compile from Linux using osxcross)
# This script builds NetLeaf for macOS x86_64 and arm64 and packages the output
#
# Usage: Run on Linux with osxcross or via WSL on Windows
#   On Windows: wsl bash build_macos.sh
#   On Linux:   ./build_macos.sh
#
# Requirements:
#   - osxcross installed at ~/osxcross
#   - macOS SDK installed

VERSION="2.4.1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
RELEASES_DIR="${SCRIPT_DIR}/releases"
OSXCROSS_DIR="${HOME}/osxcross"
OSXCROSS_TARGET_DIR="${OSXCROSS_DIR}/target"
OSXCROSS_SDK="${OSXCROSS_TARGET_DIR}/SDK/MacOSX15.5.sdk"

echo "========================================"
echo "  NetLeaf v${VERSION} Build Script (macOS)"
echo "========================================"
echo
echo "Usage: Run on Linux with osxcross or via WSL"
echo "  On Windows: wsl bash build_macos.sh [x86_64|arm64]"
echo "  On Linux:   ./build_macos.sh [x86_64|arm64]"
echo "  (no argument / 'all' builds both architectures)"
echo

# Optional: build only the requested platform
#   Usage: bash build_macos.sh <platform>   (x86_64 | arm64)
#   Default (no argument) builds both x86_64 and arm64.
REQUESTED_PLATFORM="${1:-all}"
case "${REQUESTED_PLATFORM}" in
    all|x86_64|arm64)
        ;;
    *)
        echo "[ERROR] Unknown platform: ${REQUESTED_PLATFORM}"
        echo "Usage: bash build_macos.sh [x86_64|arm64]"
        echo "  (no argument) / all : build both x86_64 and arm64 (default)"
        echo "  x86_64              : build only macOS x86_64"
        echo "  arm64               : build only macOS arm64"
        exit 1
        ;;
esac

# Check if osxcross is available
if [ ! -d "${OSXCROSS_TARGET_DIR}" ]; then
    echo "[ERROR] osxcross not found at ${OSXCROSS_TARGET_DIR}"
    echo "Please install osxcross first."
    echo "Install with:"
    echo "  git clone https://github.com/tpoechtrager/osxcross.git ~/osxcross"
    echo "  cd ~/osxcross && ./install.sh"
    exit 1
fi

if [ ! -d "${OSXCROSS_SDK}" ]; then
    echo "[ERROR] macOS SDK not found at ${OSXCROSS_SDK}"
    echo "Please install macOS SDK for osxcross."
    echo "Download from: https://github.com/alexey-lysiuk/macos-cross-toolchain"
    exit 1
fi

# Create output directories
mkdir -p "${BUILD_DIR}"
mkdir -p "${RELEASES_DIR}"

# Check cmake version (minimum 3.14)
if ! command -v cmake &> /dev/null; then
    echo -e "${RED}[ERROR] cmake not found. Please install cmake first.${NC}"
    exit 1
fi
cmake_version=$(cmake --version | head -1 | sed 's/.*\///' | cut -d' ' -f1)
cmake_major=$(echo "${cmake_version}" | cut -d. -f1)
cmake_minor=$(echo "${cmake_version}" | cut -d. -f2)
if [ "${cmake_major}" -lt 3 ] || ([ "${cmake_major}" -eq 3 ] && [ "${cmake_minor}" -lt 14 ]); then
    echo -e "${RED}[ERROR] CMake version ${cmake_version} is too old. Need CMake >= 3.14${NC}"
    echo "Upgrade CMake with:"
    echo "  sudo apt install cmake       # Ubuntu/Debian"
    echo "  brew install cmake           # macOS"
    echo "  curl -LO https://github.com/Kitware/CMake/releases/download/v3.28.3/cmake-3.28.3-linux-x86_64.tar.gz && sudo tar -xzf cmake-*.tar.gz -C /usr/local --strip-components=1"
    exit 1
fi

# Build for macOS x86_64
if [ "${REQUESTED_PLATFORM}" = "all" ] || [ "${REQUESTED_PLATFORM}" = "x86_64" ]; then
echo "[1/2] Building for macOS x86_64..."
if [ -d "${BUILD_DIR}/macos_x64" ]; then
    rm -rf "${BUILD_DIR}/macos_x64"
fi
mkdir -p "${BUILD_DIR}/macos_x64"
cd "${BUILD_DIR}/macos_x64"

export OSXCROSS_HOST=x86_64-apple-darwin21.4
export OSXCROSS_TARGET_DIR="${OSXCROSS_TARGET_DIR}"
export OSXCROSS_TARGET=darwin21.4
export OSXCROSS_SDK="${OSXCROSS_SDK}"

cmake -DCMAKE_TOOLCHAIN_FILE="${OSXCROSS_TARGET_DIR}/toolchain.cmake" \
      -DWIDE_LIB=ON \
      -DBUILD_SHARED_LIBS=ON \
      -DBUILD_EXAMPLES=ON \
      -DBUILD_TLS=ON \
      -DBUILD_MQTT=ON \
      -DBUILD_MQTT_SERVER=ON \
      "${SCRIPT_DIR}"

cmake --build . -j$(nproc)
cd "${SCRIPT_DIR}"
echo "macOS x86_64 build completed!"

# Create macOS x86_64 package
echo "Creating macOS x86_64 package..."
cd "${BUILD_DIR}/macos_x64"

PKG_TMP="pkg_tmp_x64_$$"
mkdir -p "${PKG_TMP}"

# Copy library files
for f in lib/*.dylib; do
    if [ -e "${f}" ]; then
        cp -r "${f}" "${PKG_TMP}/" 2>/dev/null || true
    fi
done

# Copy header files
for h in ../../include/netleaf*.h; do
    if [ -e "${h}" ]; then
        cp -r "${h}" "${PKG_TMP}/" 2>/dev/null || true
    fi
done

# Copy executable and source files
cp -r bin/example_all_features "${PKG_TMP}/" 2>/dev/null || true
cp -r ../../examples/example_all_features.c "${PKG_TMP}/" 2>/dev/null || true

# Create tar.gz package
tar -czf "${RELEASES_DIR}/NetLeaf-${VERSION}-macos-x86_64.tar.gz" -C "${PKG_TMP}" .
rm -rf "${PKG_TMP}"

echo "macOS x86_64 package created!"
fi
echo

# Build for macOS arm64
if [ "${REQUESTED_PLATFORM}" = "all" ] || [ "${REQUESTED_PLATFORM}" = "arm64" ]; then
echo "[2/2] Building for macOS arm64..."
if [ -d "${BUILD_DIR}/macos_arm64" ]; then
    rm -rf "${BUILD_DIR}/macos_arm64"
fi
mkdir -p "${BUILD_DIR}/macos_arm64"
cd "${BUILD_DIR}/macos_arm64"

export OSXCROSS_HOST=arm64-apple-darwin21.4

cmake -DCMAKE_TOOLCHAIN_FILE="${OSXCROSS_TARGET_DIR}/toolchain.cmake" \
      -DWIDE_LIB=ON \
      -DBUILD_SHARED_LIBS=ON \
      -DBUILD_EXAMPLES=ON \
      -DBUILD_TLS=ON \
      -DBUILD_MQTT=ON \
      -DBUILD_MQTT_SERVER=ON \
      "${SCRIPT_DIR}"

cmake --build . -j$(nproc)
cd "${SCRIPT_DIR}"
echo "macOS arm64 build completed!"

# Create macOS arm64 package
echo "Creating macOS arm64 package..."
cd "${BUILD_DIR}/macos_arm64"

PKG_TMP="pkg_tmp_arm64_$$"
mkdir -p "${PKG_TMP}"

# Copy library files
for f in lib/*.dylib; do
    if [ -e "${f}" ]; then
        cp -r "${f}" "${PKG_TMP}/" 2>/dev/null || true
    fi
done

# Copy header files
for h in ../../include/netleaf*.h; do
    if [ -e "${h}" ]; then
        cp -r "${h}" "${PKG_TMP}/" 2>/dev/null || true
    fi
done

# Copy executable and source files
cp -r bin/example_all_features "${PKG_TMP}/" 2>/dev/null || true
cp -r ../../examples/example_all_features.c "${PKG_TMP}/" 2>/dev/null || true

# Create tar.gz package
tar -czf "${RELEASES_DIR}/NetLeaf-${VERSION}-macos-arm64.tar.gz" -C "${PKG_TMP}" .
rm -rf "${PKG_TMP}"

echo "macOS arm64 package created!"
fi
echo

echo "========================================"
echo "  All builds completed successfully!"
echo "========================================"
echo
echo "Packages location: ${RELEASES_DIR}/"
if [ "${REQUESTED_PLATFORM}" = "all" ] || [ "${REQUESTED_PLATFORM}" = "x86_64" ]; then
    echo "  - NetLeaf-${VERSION}-macos-x86_64.tar.gz"
fi
if [ "${REQUESTED_PLATFORM}" = "all" ] || [ "${REQUESTED_PLATFORM}" = "arm64" ]; then
    echo "  - NetLeaf-${VERSION}-macos-arm64.tar.gz"
fi
echo
