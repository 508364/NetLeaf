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

VERSION="3.1.0"
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
    # 先清掉旧产物，避免构建失败时把上次残留的包误报为成功
    rm -f "${RELEASES_DIR}/NetLeaf-${VERSION}-macos-${arch}.tar.gz"
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
        -DBUILD_EXAMPLES=OFF \
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

    # 打包（目录结构 v3.1.0：根文档/pc + lib/bin + lib/cmake + include 平铺，
    # 包内含顶层目录 NetLeaf-<版本>-macos-<arch>/；.a 静态库不入包）
    local pkg_root="${bdir}/pkg_tmp"
    local pkg_name="NetLeaf-${VERSION}-macos-${arch}"
    local pkg="${pkg_root}/${pkg_name}"
    local out="${RELEASES_DIR}/${pkg_name}.tar.gz"
    rm -rf "$pkg_root"; mkdir -p "$pkg/lib/bin" "$pkg/include"
    shopt -s nullglob
    local f base name
    # 第200轮：符号链接不入包——只取真实文件，跳过 .dylib / .dylib.N 链接，
    # 主文件去掉版本号重命名（libnetleaf.3.1.0.dylib → libnetleaf.dylib）。
    for f in "$bdir"/lib/*.dylib "$bdir"/lib/*.dylib.*; do
        [[ -f "$f" && ! -L "$f" ]] || continue
        base="$(basename "$f")"
        stem="${base%.dylib}"
        if [[ "$stem" == *.* ]]; then
            name="${stem%%.*}.dylib"    # 去掉 .<版本号> 后缀
        else
            name="$base"
        fi
        cp -f "$f" "$pkg/lib/bin/${name}" || true
    done
    # 第200轮方案1（macOS 对称修正）：install_name 同步。
    # 与 Linux 的 NEEDED/SONAME 等价——包内库的 install_name 原为
    # @rpath/libnetleaf.3.dylib（或构建机绝对路径），重命名后引用会断，
    # 把 id 与依赖引用统一改为 @rpath/<新名>。
    # 工具解析：macOS 主机为裸名 install_name_tool/otool；osxcross 交叉环境为
    # <triplet>-install_name_tool/<triplet>-otool（优先 target/bin 绝对路径）。
    # 映射目标不在包内的（系统库 /usr/lib/libSystem.B.dylib 等）自动跳过。
    local install_tool="" otool_tool=""
    if command -v install_name_tool >/dev/null 2>&1 && command -v otool >/dev/null 2>&1; then
        install_tool="install_name_tool"
        otool_tool="otool"
    elif [[ -x "${OSXCROSS_TARGET_DIR}/bin/${triplet}-install_name_tool" && -x "${OSXCROSS_TARGET_DIR}/bin/${triplet}-otool" ]]; then
        install_tool="${OSXCROSS_TARGET_DIR}/bin/${triplet}-install_name_tool"
        otool_tool="${OSXCROSS_TARGET_DIR}/bin/${triplet}-otool"
    elif command -v "${triplet}-install_name_tool" >/dev/null 2>&1 && command -v "${triplet}-otool" >/dev/null 2>&1; then
        install_tool="${triplet}-install_name_tool"
        otool_tool="${triplet}-otool"
    fi
    if [[ -n "$install_tool" ]]; then
        local dep dst stem2
        for f in "$pkg"/lib/bin/*.dylib; do
            [[ -f "$f" ]] || continue
            "$install_tool" -id "@rpath/$(basename "$f")" "$f" 2>/dev/null || true
        done
        for f in "$pkg"/lib/bin/*.dylib; do
            [[ -f "$f" ]] || continue
            for dep in $("$otool_tool" -L "$f" | tail -n +2 | awk '{print $1}'); do
                [[ "$dep" == *.dylib ]] || continue
                stem2="$(basename "$dep")"
                stem2="${stem2%.dylib}"
                # 无版本号时 dst 即原名本身；只要映射目标在包内（含构建机
                # 绝对路径引用的包内库），统一改写为 @rpath/<新名>，
                # 系统库（libSystem.B.dylib 等）映射目标不在包内自动跳过
                dst="${stem2%%.*}.dylib"
                if [[ -f "$pkg/lib/bin/${dst}" && "$dep" != "@rpath/${dst}" ]]; then
                    "$install_tool" -change "$dep" "@rpath/${dst}" "$f" 2>/dev/null || true
                fi
            done
        done
    else
        echo "[WARN] install_name_tool/otool 不可用：包内 dylib install_name 仍带版本号"
    fi
    # 公共头文件平铺到 include/（原 include/optimize/ 下的头文件也平铺进来）
    for f in "$SCRIPT_DIR"/include/*.h "$SCRIPT_DIR"/include/optimize/*.h; do
        cp -f "$f" "$pkg/include/" 2>/dev/null || true
    done
    # 说明文档与许可证
    for f in README.md README_EN.md LICENSE; do
        cp -f "$SCRIPT_DIR/$f" "$pkg/" 2>/dev/null || true
    done
    # pkg-config（放包根，prefix=${pcfiledir} 相对定位）
    if [[ -f "$bdir/pkgmeta/netleaf.pc" ]]; then
        cp -f "$bdir/pkgmeta/netleaf.pc" "$pkg/"
    fi
    # CMake package config（放 lib/cmake/NetLeaf/，find_package 可直接定位包根）
    if [[ -f "$bdir/pkgmeta/NetLeafConfig.cmake" ]]; then
        mkdir -p "$pkg/lib/cmake/NetLeaf"
        cp -f "$bdir/pkgmeta/NetLeafConfig.cmake" \
              "$bdir/pkgmeta/NetLeafConfigVersion.cmake" \
              "$pkg/lib/cmake/NetLeaf/"
    fi
    shopt -u nullglob
    tar -czf "$out" -C "$pkg_root" "$pkg_name" 2>/dev/null || true
    rm -rf "$pkg_root"
    echo -e "${GREEN}[OK] macOS ${arch} -> ${out}${NC}"
}

FAILED=()
BUILT=()
for arch in "${TARGETS[@]}"; do
    if build_macos "$arch"; then
        BUILT+=("$arch")
    else
        FAILED+=("$arch")
    fi
done

echo
echo "========================================"
echo "  macOS build finished"
echo "========================================"
for arch in "${BUILT[@]}"; do
    echo "  ${RELEASES_DIR}/NetLeaf-${VERSION}-macos-${arch}.tar.gz"
done
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo -e "${RED}失败: ${FAILED[*]}${NC}"
    exit 1
fi
