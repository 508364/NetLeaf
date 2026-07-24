#!/bin/bash
# NetLeaf macOS Build Script (Cross-compile from Linux using osxcross)
# This script builds NetLeaf for macOS x86_64 and arm64 and packages the output

VERSION="2.2.2"
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

# Check if osxcross is available
if [ ! -d "${OSXCROSS_TARGET_DIR}" ]; then
    echo "[ERROR] osxcross not found at ${OSXCROSS_TARGET_DIR}"
    echo "Please install osxcross first."
    exit 1
fi

if [ ! -d "${OSXCROSS_SDK}" ]; then
    echo "[ERROR] macOS SDK not found at ${OSXCROSS_SDK}"
    echo "Please install macOS SDK for osxcross."
    exit 1
fi

# Create output directories
mkdir -p "${BUILD_DIR}"
mkdir -p "${RELEASES_DIR}"

# Build for macOS x86_64
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
      -DBUILD_STATIC_LIBS=ON \
      -DBUILD_EXAMPLES=ON \
      -DBUILD_AUTOCOMPLETE=ON \
      -DBUILD_AUTOROUTE=ON \
      -DBUILD_ERRORPAGE=ON \
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
for f in lib/*.dylib lib/*.a; do
    if [ -e "${f}" ]; then
        cp -r "${f}" "${PKG_TMP}/" 2>/dev/null || true
    fi
done

# Copy executable and source files
cp -r bin/example_all_features "${PKG_TMP}/" 2>/dev/null || true
cp -r ../../include/netleaf.h "${PKG_TMP}/" 2>/dev/null || true
cp -r ../../examples/example_all_features.c "${PKG_TMP}/" 2>/dev/null || true

# Create tar.gz package
tar -czf "${RELEASES_DIR}/NetLeaf-${VERSION}-macos-x86_64.tar.gz" -C "${PKG_TMP}" .
rm -rf "${PKG_TMP}"

echo "macOS x86_64 package created!"
echo

# Build for macOS arm64
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
      -DBUILD_STATIC_LIBS=ON \
      -DBUILD_EXAMPLES=ON \
      -DBUILD_AUTOCOMPLETE=ON \
      -DBUILD_AUTOROUTE=ON \
      -DBUILD_ERRORPAGE=ON \
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
for f in lib/*.dylib lib/*.a; do
    if [ -e "${f}" ]; then
        cp -r "${f}" "${PKG_TMP}/" 2>/dev/null || true
    fi
done

# Copy executable and source files
cp -r bin/example_all_features "${PKG_TMP}/" 2>/dev/null || true
cp -r ../../include/netleaf.h "${PKG_TMP}/" 2>/dev/null || true
cp -r ../../examples/example_all_features.c "${PKG_TMP}/" 2>/dev/null || true

# Create tar.gz package
tar -czf "${RELEASES_DIR}/NetLeaf-${VERSION}-macos-arm64.tar.gz" -C "${PKG_TMP}" .
rm -rf "${PKG_TMP}"

echo "macOS arm64 package created!"
echo

echo "========================================"
echo "  All builds completed successfully!"
echo "========================================"
echo
echo "Packages location: ${RELEASES_DIR}/"
echo "  - NetLeaf-${VERSION}-macos-x86_64.tar.gz"
echo "  - NetLeaf-${VERSION}-macos-arm64.tar.gz"
echo
