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

VERSION="3.0.0"
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
        return 0
    fi

    local arch_dir="${BUILD_DIR}/linux_${arch}"
    echo "----------------------------------------"
    echo -e "[Build] Linux ${name}  (CC=${cc_path})"

    rm -rf "${arch_dir}"
    mkdir -p "${arch_dir}"

    local cmake_args=(
        "-DCMAKE_BUILD_TYPE=Release"
        "-DBUILD_SHARED_LIBS=ON"
        "-DBUILD_EXAMPLES=ON"
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

# 打包该架构产物（库 + 公共头文件）
package_arch() {
    local arch="$1"
    local name="${ARCH_NAME[$arch]:-$arch}"
    local arch_dir="${BUILD_DIR}/linux_${arch}"
    local pkg_tmp="${arch_dir}/pkg_tmp"
    local out="${RELEASES_DIR}/NetLeaf-${VERSION}-linux-${arch}.tar.gz"

    rm -rf "${pkg_tmp}"
    mkdir -p "${pkg_tmp}"
    shopt -s nullglob
    local f
    for f in "${arch_dir}"/lib/*.so "${arch_dir}"/lib/*.so.* "${arch_dir}"/lib/*.a; do
        cp -f "$f" "${pkg_tmp}/" 2>/dev/null || true
    done
    for f in "${SCRIPT_DIR}"/include/*.h; do
        cp -f "$f" "${pkg_tmp}/" 2>/dev/null || true
    done
    shopt -u nullglob

    tar -czf "${out}" -C "${pkg_tmp}" . 2>/dev/null || true
    rm -rf "${pkg_tmp}"
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
for arch in "${TARGETS[@]}"; do
    if build_arch "$arch"; then
        package_arch "$arch"
        BUILT+=("$arch")
        echo
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
