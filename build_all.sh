#!/usr/bin/env bash
# ============================================================
# NetLeaf - Linux build script
#
# 本脚本只构建 Linux 目标（本机 + 可用的交叉工具链）。
# 不涉及 Windows / macOS / Android，也不会调用其它脚本。
#
# 用法：
#   ./build_all.sh [all|x86|x86_64|arm|arm64|riscv64|...]
#   默认 all：扫描本机可用的编译器并逐个构建。
#
# 产物：releases/NetLeaf-<版本>-linux-<arch>.tar.gz
# ============================================================
set -uo pipefail

VERSION="3.1.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
RELEASES_DIR="${SCRIPT_DIR}/releases"
HOST_ARCH="$(uname -m)"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

# arch -> 交叉编译器前缀（仅 Linux 目标）
declare -A CROSS_PREFIX=(
    ["x86_64"]="x86_64-linux-gnu"
    ["x86"]="i686-linux-gnu"
    ["arm"]="arm-linux-gnueabihf"
    ["arm64"]="aarch64-linux-gnu"
    ["riscv64"]="riscv64-linux-gnu"
    ["powerpc64le"]="powerpc64le-linux-gnu"
    ["powerpc64"]="powerpc64-linux-gnu"
    ["mips64el"]="mips64el-linux-gnuabi64"
    ["mips64"]="mips64-linux-gnuabi64"
    ["mipsel"]="mipsel-linux-gnu"
    ["mips"]="mips-linux-gnu"
    ["s390x"]="s390x-linux-gnu"
)

# arch -> 展示名
declare -A ARCH_NAME=(
    ["x86_64"]="x86_64"
    ["x86"]="x86"
    ["arm"]="ARM"
    ["arm64"]="ARM64"
    ["riscv64"]="RISC-V 64"
    ["powerpc64le"]="PowerPC 64 LE"
    ["powerpc64"]="PowerPC 64 BE"
    ["mips64el"]="MIPS64 EL"
    ["mips64"]="MIPS64 BE"
    ["mipsel"]="MIPS EL"
    ["mips"]="MIPS BE"
    ["s390x"]="s390x"
)

# 需要构建的架构列表（顺序即构建顺序）
ALL_ARCHS=(x86_64 x86 arm arm64 riscv64 powerpc64le powerpc64 mips64el mips64 mipsel mips s390x)

echo "========================================"
echo "  NetLeaf v${VERSION} - Linux build"
echo "========================================"
echo "Host   : ${HOST_ARCH}"
echo "Source : ${SCRIPT_DIR}"
echo "Output : ${RELEASES_DIR}/"
echo

mkdir -p "${BUILD_DIR}" "${RELEASES_DIR}"

if ! command -v cmake >/dev/null 2>&1; then
    echo -e "${RED}[ERROR] cmake not found.${NC}"
    exit 1
fi

# 返回该架构可用的编译器完整路径；不可用返回空。
# x86_64/x86 优先使用本机 gcc（x86 用 -m32），否则回退交叉编译器。
detect_cc() {
    local arch="$1"
    local prefix="${CROSS_PREFIX[$arch]:-}"
    if command -v "${prefix}-gcc" >/dev/null 2>&1; then
        command -v "${prefix}-gcc"
        return 0
    fi
    if [[ "$arch" == "x86_64" && "$HOST_ARCH" == "x86_64" ]] && command -v gcc >/dev/null 2>&1; then
        command -v gcc
        return 0
    fi
    if [[ "$arch" == "x86" ]] && command -v gcc >/dev/null 2>&1; then
        # 本机 gcc + -m32 需要 multilib 支持
        if echo 'int main(void){return 0;}' | gcc -m32 -x c - -o /dev/null >/dev/null 2>&1; then
            command -v gcc
            return 0
        fi
    fi
    return 1
}

# 额外的编译参数（仅用本机 gcc 构建 x86 时需要 -m32）
extra_flags() {
    local arch="$1"
    local cc_path="$2"
    if [[ "$arch" == "x86" && "$(basename "$cc_path")" == "gcc" ]]; then
        echo "-m32"
    else
        echo ""
    fi
}

build_arch() {
    local arch="$1"
    local name="${ARCH_NAME[$arch]:-$arch}"
    local cc_path
    if ! cc_path="$(detect_cc "$arch")"; then
        echo -e "${YELLOW}[SKIP] Linux ${name}: no compiler available${NC}"
        return 2   # 2 = 跳过（区别于失败），调用方不应打包
    fi

    local arch_dir="${BUILD_DIR}/linux_${arch}"
    echo "----------------------------------------"
    echo -e "[Build] Linux ${name}  (CC=${cc_path})"

    rm -rf "${arch_dir}"
    mkdir -p "${arch_dir}"

    local cmake_args=(
        "-DCMAKE_BUILD_TYPE=Release"
        "-DBUILD_SHARED_LIBS=ON"
        "-DBUILD_EXAMPLES=OFF"
        "-DBUILD_TESTS=OFF"
        "-DBUILD_TLS=OFF"
        "-DBUILD_TLS3=ON"
        "-DBUILD_LANG=ON"
        "-DBUILD_MQTT=ON"
        "-DBUILD_MQTT_SERVER=ON"
        "-DCMAKE_C_COMPILER=${cc_path}"
    )
    local flags
    flags="$(extra_flags "$arch" "$cc_path")"
    if [[ -n "$flags" ]]; then
        cmake_args+=("-DCMAKE_C_FLAGS=${flags}")
        cmake_args+=("-DCMAKE_EXE_LINKER_FLAGS=${flags}")
        cmake_args+=("-DCMAKE_SHARED_LINKER_FLAGS=${flags}")
    fi
    # 交叉编译：显式指定目标系统，排除 host 上的探测结果
    if [[ "$arch" != "${HOST_ARCH}" ]]; then
        cmake_args+=("-DCMAKE_SYSTEM_NAME=Linux")
        cmake_args+=("-DCMAKE_SYSTEM_PROCESSOR=${arch}")
        local ar_path
        if command -v "$(basename "${cc_path%-gcc}")-ar" >/dev/null 2>&1; then
            ar_path="$(command -v "$(basename "${cc_path%-gcc}")-ar")"
            cmake_args+=("-DCMAKE_AR=${ar_path}")
        fi
    fi

    if ! cmake -S "${SCRIPT_DIR}" -B "${arch_dir}" "${cmake_args[@]}"; then
        echo -e "${RED}[FAIL] CMake configure failed for ${name}${NC}"
        return 1
    fi
    if ! cmake --build "${arch_dir}" --parallel "$(nproc 2>/dev/null || echo 4)"; then
        echo -e "${RED}[FAIL] Build failed for ${name}${NC}"
        return 1
    fi
    echo -e "${GREEN}[OK] Linux ${name} built${NC}"
    return 0
}

# 打包该架构产物（目录结构 v3.1.0：
#   <包根>/LICENSE、README*.md、netleaf.pc
#   <包根>/lib/bin/     共享库（不打包 .a 静态库）
#   <包根>/lib/cmake/NetLeaf/  CMake package config
#   <包根>/include/     公共头文件（平铺，含原 optimize/ 下头文件）
#   包内含顶层目录 NetLeaf-<版本>-linux-<arch>/）
package_arch() {
    local arch="$1"
    local name="${ARCH_NAME[$arch]:-$arch}"
    local arch_dir="${BUILD_DIR}/linux_${arch}"
    local pkg_root="${arch_dir}/pkg_tmp"
    local pkg_name="NetLeaf-${VERSION}-linux-${arch}"
    local pkg="${pkg_root}/${pkg_name}"
    local out="${RELEASES_DIR}/${pkg_name}.tar.gz"

    rm -rf "${pkg_root}"
    mkdir -p "${pkg}/lib/bin" "${pkg}/include"
    shopt -s nullglob
    local f base name
    # 第200轮：符号链接不入包——只取真实文件，跳过 .so / .so.N 链接，
    # 主文件 .so.<版本号> 去掉版本后缀重命名为 .so
    # （如 libnetleaf.so.3.1.0 → libnetleaf.so、libnetleaf_mqtt.so.3.1.0 → libnetleaf_mqtt.so）。
    # 只打包共享库，.a 静态库（mbedtls/p256m 等）不再入包。
    for f in "${arch_dir}"/lib/*.so "${arch_dir}"/lib/*.so.*; do
        [[ -f "$f" && ! -L "$f" ]] || continue
        base="$(basename "$f")"
        if [[ "$base" == *.so.* ]]; then
            name="${base%%.so.*}.so"      # 去掉 .so.<版本号> 后缀
        else
            name="$base"
        fi
        cp -f "$f" "${pkg}/lib/bin/${name}" || true
    done
    # 第200轮方案1：库名去版本后，用 patchelf 同步 SONAME / NEEDED / RUNPATH。
    # 不同步则扩展的 NEEDED 仍是 libnetleaf.so.3，包内已无此文件，干净机器上
    # 扩展加载必失败；RUNPATH 原为构建机绝对路径，一并改为 $ORIGIN（同目录），
    # 包解压到任意位置均可自洽使用。系统库（libc.so.6 等）不在包内，映射目标
    # 不存在即跳过，不受影响。
    if command -v patchelf >/dev/null 2>&1; then
        local dep newdep sn
        for f in "${pkg}"/lib/bin/*.so; do
            [[ -f "$f" && ! -L "$f" ]] || continue
            sn="$(patchelf --print-soname "$f" 2>/dev/null || true)"
            if [[ -n "$sn" && "$sn" == *.so.* ]]; then
                newdep="${sn%%.so.*}.so"
                if [[ -f "${pkg}/lib/bin/${newdep}" ]]; then
                    patchelf --set-soname "$newdep" "$f"
                fi
            fi
            for dep in $(patchelf --print-needed "$f" 2>/dev/null); do
                if [[ "$dep" == *.so.* ]]; then
                    newdep="${dep%%.so.*}.so"
                    if [[ "$newdep" != "$dep" && -f "${pkg}/lib/bin/${newdep}" ]]; then
                        patchelf --replace-needed "$dep" "$newdep" "$f"
                    fi
                fi
            done
            patchelf --set-rpath '$ORIGIN' "$f"
        done
    else
        # 回退（环境无 patchelf）：无法改写 SONAME/NEEDED，改为按源文件的原
        # SONAME 在包内补建相对符号链接（如 libnetleaf.so.3 → libnetleaf.so），
        # 使扩展的 NEEDED 与主程序链接时记录的 SONAME 仍能解析到主文件，
        # 防止用本包编译/运行时加载失败。RUNPATH 仍为构建机路径，
        # 使用时需以 LD_LIBRARY_PATH 指向包内 lib/bin。
        echo "[WARN] patchelf 未安装：改用 SONAME 符号链接回退（安装 patchelf 可获得完全自洽的包）"
        if command -v readelf >/dev/null 2>&1; then
            local srcf sn
            for srcf in "${arch_dir}"/lib/*.so "${arch_dir}"/lib/*.so.*; do
                [[ -f "$srcf" && ! -L "$srcf" ]] || continue
                base="$(basename "$srcf")"
                if [[ "$base" == *.so.* ]]; then name="${base%%.so.*}.so"; else name="$base"; fi
                sn="$(readelf -d "$srcf" 2>/dev/null | grep '(SONAME)' | grep -o '\[[^]]*\]' | head -n1)"
                sn="${sn#\[}"
                sn="${sn%\]}"
                if [[ -n "$sn" && "$sn" != "$name" && -f "${pkg}/lib/bin/${name}" && ! -e "${pkg}/lib/bin/${sn}" ]]; then
                    ln -s "$name" "${pkg}/lib/bin/${sn}"
                fi
            done
        else
            echo "[WARN] readelf 不可用，无法创建 SONAME 符号链接回退"
        fi
    fi
    # 公共头文件平铺到 include/（原 include/optimize/ 下的头文件也平铺进来，
    # 包内头文件互相包含均为同目录引号包含，平铺后依然可用）
    for f in "${SCRIPT_DIR}"/include/*.h "${SCRIPT_DIR}"/include/optimize/*.h; do
        cp -f "$f" "${pkg}/include/" 2>/dev/null || true
    done
    # 说明文档与许可证
    for f in README.md README_EN.md LICENSE; do
        cp -f "${SCRIPT_DIR}/${f}" "${pkg}/" 2>/dev/null || true
    done
    # pkg-config（放包根，prefix=${pcfiledir} 相对定位）
    if [[ -f "${arch_dir}/pkgmeta/netleaf.pc" ]]; then
        cp -f "${arch_dir}/pkgmeta/netleaf.pc" "${pkg}/"
    fi
    # CMake package config（放 lib/cmake/NetLeaf/，find_package 可直接定位包根）
    if [[ -f "${arch_dir}/pkgmeta/NetLeafConfig.cmake" ]]; then
        mkdir -p "${pkg}/lib/cmake/NetLeaf"
        cp -f "${arch_dir}/pkgmeta/NetLeafConfig.cmake" \
              "${arch_dir}/pkgmeta/NetLeafConfigVersion.cmake" \
              "${pkg}/lib/cmake/NetLeaf/"
    fi
    shopt -u nullglob

    tar -czf "${out}" -C "${pkg_root}" "${pkg_name}" 2>/dev/null || true
    rm -rf "${pkg_root}"
    echo -e "${GREEN}[Pack] ${out}${NC}"
}

# 解析目标参数
TARGETS=()
if [[ $# -ge 1 && "$1" != "all" ]]; then
    if [[ -z "${ARCH_NAME[$1]:-}" ]]; then
        echo -e "${RED}[ERROR] Unknown linux arch: $1${NC}"
        echo "Supported: all ${ALL_ARCHS[*]}"
        exit 2
    fi
    TARGETS=("$1")
else
    TARGETS=("${ALL_ARCHS[@]}")
fi

BUILT=()
SKIPPED=()
FAILED=()
for arch in "${TARGETS[@]}"; do
    build_arch "$arch"
    rc=$?
    if [[ $rc -eq 0 ]]; then
        package_arch "$arch"
        BUILT+=("$arch")
        echo
    elif [[ $rc -eq 2 ]]; then
        SKIPPED+=("$arch")
    else
        FAILED+=("$arch")
    fi
done

echo "========================================"
echo -e "${GREEN}  Linux build finished${NC}"
echo "========================================"
if [[ ${#BUILT[@]} -eq 0 ]]; then
    echo -e "${YELLOW}No architecture was built (no compiler found).${NC}"
    exit 1
fi
echo "Packages:"
for arch in "${BUILT[@]}"; do
    echo "  - ${RELEASES_DIR}/NetLeaf-${VERSION}-linux-${arch}.tar.gz"
done
if [[ ${#SKIPPED[@]} -gt 0 ]]; then
    echo -e "${YELLOW}Skipped (no compiler): ${SKIPPED[*]}${NC}"
fi
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo -e "${RED}Failed: ${FAILED[*]}${NC}"
    exit 1
fi
