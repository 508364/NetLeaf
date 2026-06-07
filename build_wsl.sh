#!/bin/bash
# NetLeaf WSL/Linux Build Script

# Set UTF-8 encoding
export LC_ALL=C.UTF-8
export LANG=C.UTF-8

echo "========================================"
echo "  NetLeaf v2.0.0 Build Script (WSL/Linux)"
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

# Create build directory
mkdir -p build_wsl
cd build_wsl || exit 1

# Configure and build
echo "Configuring CMake for Linux..."
cmake .. -DCMAKE_BUILD_TYPE=Release
if [ $? -ne 0 ]; then
    echo "CMake configuration failed!"
    exit 1
fi

echo ""
echo "Building NetLeaf..."
cmake --build . --config Release
if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo ""
echo "========================================"
echo "  Build completed successfully!"
echo "========================================"
echo ""
echo "Output directory: $(pwd)/bin"
echo "Libraries: $(pwd)/lib"
echo ""
