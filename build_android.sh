#!/usr/bin/env bash
# ============================================================
# NetLeaf - Android 交叉编译构建脚本
#
# 本脚本只构建 Android 目标（NDK clang），不涉及 Windows / Linux / macOS，
# 也不会调用其它脚本。
#
# 用法：
#   ./build_android.sh [aarch64|armeabi-v7a|x86_64|all]   （默认 all）
#
# 环境变量：
#   ANDROID_NDK / ANDROID_NDK_HOME / ANDROID_NDK_ROOT / NDK_HOME / NDK_ROOT
#   ANDROID_HOME / ANDROID_SDK_ROOT（从中派生 <sdk>/ndk）
#
# 产物：build-android/<abi>/stage-lib/
#   libnetleaf*.so + 公共头文件（可直接拷入 jniLibs/<abi>/）
# ============================================================
set -uo pipefail

VERSION="3.0.0"
MIN_SDK="21"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${SCRIPT_DIR}/build-android"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

# ---------- 参数 ----------
REQUESTED="all"
if [[ $# -ge 1 ]]; then
    case "$1" in
        aarch64|armeabi-v7a|x86_64) REQUESTED="$1" ;;
        all) REQUESTED="all" ;;
        -h|--help)
            grep -E '^# ' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo -e "${RED}未知目标: $1${NC} （可选 aarch64 / armeabi-v7a / x86_64 / all）"
            exit 2 ;;
    esac
fi
if [[ "$REQUESTED" == "all" ]]; then
    TARGETS=(aarch64 armeabi-v7a x86_64)
else
    TARGETS=("$REQUESTED")
fi

# ---------- 工具检查 ----------
if ! command -v cmake >/dev/null 2>&1; then
    echo -e "${RED}[ERROR] cmake not found.${NC}"
    exit 1
fi
CMAKE_VER="$(cmake --version | head -n1 | awk '{print $3}')"
CMAKE_MAJOR="${CMAKE_VER%%.*}"; CMAKE_REST="${CMAKE_VER#*.}"; CMAKE_MINOR="${CMAKE_REST%%.*}"
if [[ "${CMAKE_MAJOR:-0}" -lt 3 ]] || { [[ "${CMAKE_MAJOR:-0}" -eq 3 ]] && [[ "${CMAKE_MINOR:-0}" -lt 22 ]]; }; then
    echo -e "${YELLOW}[WARN] cmake ${CMAKE_VER} detected; Android NDK r25+ recommends cmake >= 3.22.${NC}"
fi

# ---------- 探测 NDK ----------
NDK_ROOT=""
for ev in ANDROID_NDK ANDROID_NDK_HOME ANDROID_NDK_ROOT NDK_HOME NDK_ROOT; do
    v="${!ev:-}"
    if [[ -n "$v" && -d "$v" ]]; then
        NDK_ROOT="$v"
        echo "  [NDK] 环境变量 $ev = $v"
        break
    fi
done

# <sdk>/ndk 可能是版本子目录集合，取版本号最大的
pick_latest_ndk() {
    local parent="$1"
    [[ -d "$parent" ]] || return 1
    local best
    best="$(ls -1 "$parent" 2>/dev/null | grep -E '^[0-9]' | sort -V | tail -n1)"
    if [[ -n "$best" && -f "$parent/$best/source.properties" ]]; then
        echo "$parent/$best"
        return 0
    fi
    return 1
}

if [[ -n "$NDK_ROOT" && ! -f "$NDK_ROOT/source.properties" ]]; then
    # 环境变量给的是包含版本子目录的父目录时，取最高版本
    if picked="$(pick_latest_ndk "$NDK_ROOT")"; then
        NDK_ROOT="$picked"
        echo "  [NDK] 选中版本目录 $picked"
    fi
fi

if [[ -z "$NDK_ROOT" ]]; then
    for p in \
        "${ANDROID_HOME:-}/ndk" "${ANDROID_SDK_ROOT:-}/ndk" \
        "$HOME/Android/Sdk/ndk" "$HOME/Library/Android/sdk/ndk" \
        "$HOME/AppData/Local/Android/Sdk/ndk"; do
        [[ -d "$p" ]] || continue
        if picked="$(pick_latest_ndk "$p")"; then
            NDK_ROOT="$picked"
            echo "  [NDK] 在 $p 中选中 $(basename "$picked")"
            break
        fi
    done
fi

if [[ -z "$NDK_ROOT" || ! -f "$NDK_ROOT/source.properties" ]]; then
    echo -e "${RED}[ERROR] 未找到有效的 Android NDK。${NC}"
    echo "  请设置 ANDROID_NDK_HOME 指向 NDK 根目录，或安装到 \$HOME/Android/Sdk/ndk。"
    exit 1
fi
TOOLCHAIN="$NDK_ROOT/build/cmake/android.toolchain.cmake"
if [[ ! -f "$TOOLCHAIN" ]]; then
    echo -e "${RED}[ERROR] 缺少 $TOOLCHAIN${NC}（需要 NDK r21+）"
    exit 1
fi

echo "========================================"
echo "  NetLeaf v${VERSION} - Android build"
echo "========================================"
echo "NDK      : $NDK_ROOT"
echo "Toolchain: $TOOLCHAIN"
echo "minSdk   : $MIN_SDK"
echo "ABI      : $REQUESTED"
echo

declare -A NDK_ARCH=( [aarch64]=arm64-v8a [armeabi-v7a]=armeabi-v7a [x86_64]=x86_64 )

FAILED=()
for abi in "${TARGETS[@]}"; do
    ABI_LABEL="${NDK_ARCH[$abi]}"
    BDIR="${BUILD_ROOT}/${abi}"
    STAGE="${BDIR}/stage-lib"

    echo "----------------------------------------"
    echo "[Build] $abi ($ABI_LABEL)"
    rm -rf "$BDIR"
    mkdir -p "$BDIR"

    if ! cmake -S "$SCRIPT_DIR" -B "$BDIR" \
        -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release \
        -DANDROID_ABI="$ABI_LABEL" \
        -DANDROID_PLATFORM="android-${MIN_SDK}" \
        -DANDROID_ARM_NEON=ON \
        -DBUILD_TESTS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_TLS=OFF \
        -DBUILD_TLS3=ON \
        -DBUILD_MQTT=OFF \
        -DBUILD_MQTT_SERVER=OFF \
        -DBUILD_LANG=ON \
        -DCMAKE_C_FLAGS="-fPIE -fdata-sections -ffunction-sections" \
        -DCMAKE_SHARED_LINKER_FLAGS="-fdata-sections -ffunction-sections -Wl,--gc-sections"; then
        echo -e "${RED}[FAIL] configure failed: $abi${NC}"
        FAILED+=("$abi")
        continue
    fi

    if ! cmake --build "$BDIR" --parallel "$(nproc 2>/dev/null || echo 4)" --target netleaf_core; then
        echo -e "${RED}[FAIL] build failed: $abi${NC}"
        FAILED+=("$abi")
        continue
    fi

    mkdir -p "$STAGE"
    shopt -s nullglob
    for f in "$BDIR"/lib/*netleaf*.so; do
        cp -f "$f" "$STAGE/" || true
    done
    for f in "$SCRIPT_DIR"/include/*.h; do
        cp -f "$f" "$STAGE/" || true
    done
    shopt -u nullglob
    echo -e "${GREEN}[OK] $abi -> $STAGE${NC}"
done

echo
echo "========================================"
echo "  Android build finished"
echo "========================================"
for abi in "${TARGETS[@]}"; do
    [[ -d "${BUILD_ROOT}/${abi}/stage-lib" ]] && echo "  ${BUILD_ROOT}/${abi}/stage-lib/"
done
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo -e "${RED}失败: ${FAILED[*]}${NC}"
    exit 1
fi
