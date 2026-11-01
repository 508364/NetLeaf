#!/bin/bash
# ============================================================
# NetLeaf - macOS build script (osxcross cross-compilation)
#
# 本脚本只构建 macOS 目标，不涉及 Windows / Linux / Android，
# 也不会调用其它脚本。
#
# 用法：
#   ./build_macos.sh [x86_64|arm64|all]   （默认 all）
#
# 依赖：osxcross + 一份 macOS SDK
# 可通过环境变量覆盖：
#   OSXCROSS_DIR        osxcross 根目录（默认 ~/osxcross）
#   OSXCROSS_TARGET_DIR osxcross target 目录（默认 $OSXCROSS_DIR/target）
#   OSXCROSS_SDK        macOS SDK 目录（默认 $OSXCROSS_TARGET_DIR/SDK/<最新 .sdk>）
#   OSXCROSS_TARGET    目标三元组版本（默认 darwin21.4）
#
# 产物：releases/NetLeaf-<版本>-macos-<arch>.tar.gz
# ============================================================
set -uo pipefail

VERSION="3.0.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
RELEASES_DIR="${SCRIPT_DIR}/releases"

OSXCROSS_DIR="${OSXCROSS_DIR:-$HOME/osxcross}"
OSXCROSS_TARGET_DIR="${OSXCROSS_TARGET_DIR:-$OSXCROSS_DIR/target}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

REQUESTED="${1:-all}"
case "$REQUESTED" in
    all|x86_64|arm64) ;;
    -h|--help) grep -E '^# ' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo -e "${RED}未知目标: $REQUESTED${NC}（可选 x86_64 / arm64 / all）"; exit 2 ;;
esac
if [[ "$REQUESTED" == "all" ]]; then
    TARGETS=(x86_64 arm64)
else
    TARGETS=("$REQUESTED")
fi

echo "========================================"
echo "  NetLeaf v${VERSION} - macOS build"
echo "========================================"
echo "osxcross: ${OSXCROSS_TARGET_DIR}"
echo "Target  : ${REQUESTED}"
echo

if [[ ! -d "$OSXCROSS_TARGET_DIR" ]]; then
    echo -e "${RED}[ERROR] osxcross not found at ${OSXCROSS_TARGET_DIR}${NC}"
    echo "  安装 osxcross 后重新运行，或设置 OSXCROSS_DIR / OSXCROSS_TARGET_DIR。"
    exit 1
fi
TOOLCHAIN="${OSXCROSS_TARGET_DIR}/toolchain.cmake"
if [[ ! -f "$TOOLCHAIN" ]]; then
    echo -e "${RED}[ERROR] $TOOLCHAIN not found.${NC}"
    exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
    echo -e "${RED}[ERROR] cmake not found.${NC}"
    exit 1
fi

mkdir -p "$BUILD_DIR" "$RELEASES_DIR"

# 自动挑选 SDK（未显式指定时取最新）
if [[ -z "${OSXCROSS_SDK:-}" ]]; then
    SDK_DIR="$(ls -1d "$OSXCROSS_TARGET_DIR"/SDK/*.sdk 2>/dev/null | sort -V | tail -n1)"
    if [[ -z "$SDK_DIR" ]]; then
        echo -e "${RED}[ERROR] macOS SDK not found under ${OSXCROSS_TARGET_DIR}/SDK${NC}"
        exit 1
    fi
    export OSXCROSS_SDK="$SDK_DIR"
fi
echo "SDK     : $OSXCROSS_SDK"
echo

build_macos() {
    local arch="$1"
    # arch -> osxcross 三元组（目标版本可用 OSXCROSS_TARGET 覆盖）
    local triplet
    local target="${OSXCROSS_TARGET:-darwin21.4}"
    case "$arch" in
        x86_64) triplet="x86_64-apple-${target}" ;;
        arm64)  triplet="arm64-apple-${target}" ;;
        *) echo -e "${RED}[ERROR] unknown macOS arch: $arch${NC}"; return 1 ;;
    esac

    local bdir="${BUILD_DIR}/macos_${arch}"
    echo "----------------------------------------"
    echo "[Build] macOS ${arch} (${triplet})"
    rm -rf "$bdir"
    mkdir -p "$bdir"

    export OSXCROSS_HOST="$triplet"
    export OSXCROSS_TARGET_DIR
    export OSXCROSS_SDK
    # osxcross 自带的 toolchain.cmake 要求这 4 个变量都存在，否则直接 FATAL_ERROR
    export OSXCROSS_TARGET="${target}"

    if ! cmake -S "$SCRIPT_DIR" -B "$bdir" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_EXAMPLES=ON \
        -DBUILD_TESTS=OFF \
        -DBUILD_TLS=OFF \
        -DBUILD_TLS3=ON \
        -DBUILD_LANG=ON \
        -DBUILD_MQTT=ON \
        -DBUILD_MQTT_SERVER=ON; then
        echo -e "${RED}[FAIL] configure failed: macOS ${arch}${NC}"
        return 1
    fi
    if ! cmake --build "$bdir" --parallel "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"; then
        echo -e "${RED}[FAIL] build failed: macOS ${arch}${NC}"
        return 1
    fi

    # 打包
    local pkg="${bdir}/pkg_tmp"
    rm -rf "$pkg"; mkdir -p "$pkg"
    shopt -s nullglob
    local f
    for f in "$bdir"/lib/*.dylib "$bdir"/lib/*.a; do
        cp -f "$f" "$pkg/" 2>/dev/null || true
    done
    for f in "$SCRIPT_DIR"/include/*.h; do
        cp -f "$f" "$pkg/" 2>/dev/null || true
    done
    shopt -u nullglob
    tar -czf "${RELEASES_DIR}/NetLeaf-${VERSION}-macos-${arch}.tar.gz" -C "$pkg" . 2>/dev/null || true
    rm -rf "$pkg"
    echo -e "${GREEN}[OK] macOS ${arch} -> ${RELEASES_DIR}/NetLeaf-${VERSION}-macos-${arch}.tar.gz${NC}"
}

FAILED=()
for arch in "${TARGETS[@]}"; do
    build_macos "$arch" || FAILED+=("$arch")
done

echo
echo "========================================"
echo "  macOS build finished"
echo "========================================"
for arch in "${TARGETS[@]}"; do
    [[ -f "${RELEASES_DIR}/NetLeaf-${VERSION}-macos-${arch}.tar.gz" ]] && \
        echo "  ${RELEASES_DIR}/NetLeaf-${VERSION}-macos-${arch}.tar.gz"
done
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo -e "${RED}失败: ${FAILED[*]}${NC}"
    exit 1
fi
