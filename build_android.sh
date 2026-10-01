#!/usr/bin/env bash
# ============================================================
# NetLeaf - Android 移动端交叉编译构建脚本
# 目标产物：libnetleaf*.so（按 ABI 分目录放入 build-android/<abi>/lib/）
#
# 用法：
#   ./build_android.sh [aarch64 | armeabi-v7a | x86_64 | all]
#   （不传参数 = all）
#
# 环境变量（可覆盖默认探测）：
#   ANDROID_NDK  —— NDK 根目录（默认依次探测下列候选路径）
#   ANDROID_SDK —— SDK 根目录（仅用于提示；NDK 自带完整工具链）
#
# 依赖：
#   - 一个 CMake 支持 Android 工具链的版本（>= 3.6，推荐 3.22+）
#   - NDK（aarch64 / armeabi-v7a / x86_64 三套 clang 交叉编译器）
#
# 本脚本不修改任何源文件；仅配置独立的 build-android/<abi> 构建树，
# 最终将产物拷贝到 build-android/<abi>/lib/。
# ============================================================

set -euo pipefail

# ---------- 默认值 ----------
VERSION="2.4.2"
MIN_SDK="21"          # Android 5.0，覆盖 99%+ 现网设备
TARGETS=(aarch64 armeabi-v7a x86_64)
BUILD_ROOT="build-android"

# ---------- 解析参数 ----------
REQUESTED_TARGET="all"
if [[ $# -ge 1 ]]; then
    case "$1" in
        aarch64|armeabi-v7a|x86_64|all)
            REQUESTED_TARGET="$1"
            ;;
        -h|--help)
            grep -E '^# |^#' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "未知目标: $1 （可选 aarch64 / armeabi-v7a / x86_64 / all）" >&2
            exit 2
            ;;
    esac
fi
if [[ "$REQUESTED_TARGET" == "all" ]]; then
    TARGETS=(aarch64 armeabi-v7a x86_64)
else
    TARGETS=("$REQUESTED_TARGET")
fi

# ---------- 探测 NDK ----------
NDK_ROOT="${ANDROID_NDK:-}"
if [[ -z "$NDK_ROOT" ]]; then
    # 候选：先试"父级目录"（含多个 NDK 版本子目录，挑最新的），再试"NDK 根目录"。
    # 覆盖：Windows 本机 (E:\...)、WSL 挂载 (/mnt/e/...)、Linux 原生
    #       (~/.android/...、/opt/...)
    parent_candidates=(
        "$HOME/Android/Sdk/ndk"
        "$HOME/AppData/Local/Android/Sdk/ndk"
        "/usr/local/android/ndk"
        "/usr/local/lib/android/ndk"
        "/opt/android/ndk"
        "/opt/ndk"
        "E:/AndroidSdk/ndk"
        "E:/AndroidNDK"
        "C:/AndroidSdk/ndk"
        "C:/Program Files/Android/Android Studio/prebuilt/ndk"
        "/mnt/e/AndroidSdk/ndk"
        "/mnt/e/AndroidNDK"
        "/mnt/c/AndroidSdk/ndk"
        "/mnt/c/AndroidNDK"
    )
    direct_candidates=(
        "$HOME/Android/Sdk/ndk/27.2.12479018"
        "$HOME/Android/Sdk/ndk/25.2.9519653"
        "$HOME/Android/Sdk/ndk/25.1.8937393"
        "$HOME/Android/Sdk/ndk/24.0.8215888"
        "$HOME/Android/Sdk/ndk/23.1.7779620"
        "E:/AndroidSdk/ndk/27.2.12479018"
        "E:/AndroidSdk/ndk/25.2.9519653"
        "E:/AndroidSdk/ndk/25.1.8937393"
        "/mnt/e/AndroidSdk/ndk/27.2.12479018"
        "/mnt/e/AndroidSdk/ndk/25.2.9519653"
    )

    # 1) 父级目录探测：找含"版本号子目录"中版本号最大的（取前三段 a.b.c 比较）
     if [[ -z "$NDK_ROOT" ]]; then
         for p in "${parent_candidates[@]}"; do
             [[ -d "$p" ]] || continue
             sub_dir=""
             best_v1=0 best_v2=0 best_v3=0
             for sub in "$p"/*/; do
                 [[ -d "$sub" ]] || continue
                 name="$(basename "$sub")"
                 # 仅匹配形如 a.b.c... 的数字版本目录
                 if [[ ! "$name" =~ ^[0-9] ]]; then continue; fi
                 if [[ -d "$sub/toolchains" || -f "$sub/ndk-build" || -d "$sub/build" ]]; then
                    IFS='.' read -r v1 v2 v3 _ <<< "$name"
                    v1="${v1:-0}"; v2="${v2:-0}"; v3="${v3:-0}"
                    if (( v1 > best_v1 ||
                          (v1 == best_v1 && v2 > best_v2) ||
                          (v1 == best_v1 && v2 == best_v2 && v3 > best_v3) )); then
                        sub_dir="$sub"
                        best_v1=$v1 best_v2=$v2 best_v3=$v3
                    fi
                fi
            done
            if [[ -n "$sub_dir" ]]; then
                NDK_ROOT="${sub_dir%/}"
                echo "  [NDK] 在 $p 中发现 NDK ${best_v1}.${best_v2}.${best_v3}"
                break
            fi
        done
    fi

    # 2) 直接目录探测（明确版本路径）
    if [[ -z "$NDK_ROOT" ]]; then
        for d in "${direct_candidates[@]}"; do
            if [[ -d "$d" && ( -d "$d/toolchains" || -f "$d/ndk-build" || -d "$d/build" ) ]]; then
                NDK_ROOT="$d"
                echo "  [NDK] 直接命中 $d"
                break
            fi
        done
    fi

    # 3) 环境变量兜底（NDK_ROOT / NDK_HOME / ANDROID_NDK_ROOT）
    if [[ -z "$NDK_ROOT" ]]; then
        for ev in NDK_ROOT NDK_HOME ANDROID_NDK_ROOT ANDROID_NDK; do
            v="${!ev:-}"
            if [[ -n "$v" && -d "$v" ]]; then
                NDK_ROOT="$v"
                echo "  [NDK] 使用环境变量 $ev = $v"
                break
            fi
        done
    fi
fi

if [[ -z "$NDK_ROOT" ]]; then
    echo "未找到 Android NDK。" >&2
    echo "请通过任一方式提供 NDK 路径：(1) 设置环境变量 ANDROID_NDK=..." >&2
    echo "(2) 将 NDK 安装到下列候选之一：" >&2
    for c in \
        "$HOME/Android/Sdk/ndk" \
        "$HOME/AppData/Local/Android/Sdk/ndk" \
        "E:/AndroidSdk/ndk" \
        "C:/AndroidSdk/ndk"; do
        echo "    - $c" >&2
    done
    exit 1
fi
NDK_ROOT="$(cd "$NDK_ROOT" && pwd)"
echo "NDK_ROOT = $NDK_ROOT"

# NDK 25+ 起，CMake toolchain 文件路径为：
#   <ndk>/build/cmake/android.toolchain.cmake
TOOLCHAIN="$NDK_ROOT/build/cmake/android.toolchain.cmake"
if [[ ! -f "$TOOLCHAIN" ]]; then
    # 旧版 NDK 走 toolchain/ 下
    TOOLCHAIN="$NDK_ROOT/build/cmake/android.toolchain.cmake"  # 同路径兜底
fi
if [[ ! -f "$TOOLCHAIN" ]]; then
    echo "错误：$TOOLCHAIN 不存在（NDK 版本可能过旧）。请使用 NDK >= 21 且推荐 25+。" >&2
    exit 1
fi
echo "TOOLCHAIN = $TOOLCHAIN"

# ABI -> NDK 架构名 + CMake 默认 ABI 名
declare -A NDK_ARCH=(
    [aarch64]=aarch64
    [armeabi-v7a]=armv7a
    [x86_64]=x86_64
)
declare -A CMAKE_ABI=(
    [aarch64]=arm64-v8a
    [armeabi-v7a]=armeabi-v7a
    [x86_64]=x86_64
)

# ---------- 逐 ABI 构建 ----------
for abi in "${TARGETS[@]}"; do
    BDIR="$BUILD_ROOT/$abi"
    ARCH="${NDK_ARCH[$abi]}"
    ABI_LABEL="${CMAKE_ABI[$abi]}"

    echo "============================================================"
    echo "[NetLeaf Android] 构建 ABI: $abi ($ABI_LABEL, arch=$ARCH)"
    echo "============================================================"

    rm -rf "$BDIR"
    mkdir -p "$BDIR"

    # minSdk：Android 5.0（API 21）。如需更高请改这里或加 -DANDROID_PLATFORM=android-NN
    ANDROID_PLATFORM="android-$MIN_SDK"

    cmake -G "Unix Makefiles" \
        -S . \
        -B "$BDIR" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release \
        -DANDROID_ABI="$ABI_LABEL" \
        -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
        -DANDROID_STL=c++_shared \
        -DANDROID_ARM_NEON=ON \
        -DBUILD_TESTS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_TLS=OFF \
        -DBUILD_TLS3=ON \
        -DBUILD_MQTT=OFF \
        -DBUILD_MQTT_SERVER=OFF \
        -DBUILD_LANG=ON \
        -DCMAKE_C_FLAGS="-fPIE -fdata-sections -ffunction-sections" \
        -DCMAKE_SHARED_LINKER_FLAGS="-fdata-sections -ffunction-sections -Wl,--gc-sections" \
        -DCMAKE_SKIP_BUILD_RPATH=ON \
        -DCMAKE_INSTALL_PREFIX="$BDIR/stage" \
        "$@" 2>&1 | tail -n 40

    make -C "$BDIR" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" netleaf_core

    echo "[NetLeaf Android] ABI=$abi 编译完成。产物："
    ls -l "$BDIR/lib/"*.so 2>/dev/null || true

    # 复制 stage 安装到 build-android/<abi>/lib/（便于直接拷入 APK 的 jniLibs/）
    mkdir -p "$BDIR/stage-lib"
    cp -f "$BDIR"/lib/*.so "$BDIR/stage-lib/" 2>/dev/null || true
    echo "  已复制产物到 $BDIR/stage-lib/"
done

echo ""
echo "============================================================"
echo "[NetLeaf Android] 构建完成。"
echo "============================================================"
for abi in "${TARGETS[@]}"; do
    ABI_LABEL="${CMAKE_ABI[$abi]}"
    BDIR="$BUILD_ROOT/$abi"
    echo "  $abi ($ABI_LABEL):"
    echo "    构建树：$BDIR"
    echo "    产物目录：$BDIR/stage-lib/   （拷贝到 Android 工程的 jniLibs/$ABI_LABEL/）"
    # 统计 so
    if [[ -d "$BDIR/stage-lib" ]]; then
        find "$BDIR/stage-lib" -name "*.so" -type f -printf "      %p  (%s 字节)\n"
    fi
done
echo "提示：将各 ABI 的 .so 放入 Android 工程的 src/main/jniLibs/<abi>/，"
echo "     并链接 -lnetleaf。核心库导出 netleaf_* 接口；扩展（tls3/lang）按需加载。"
echo "minSdk=$MIN_SDK (Android 5.0)。"
