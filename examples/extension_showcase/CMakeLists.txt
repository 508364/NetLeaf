# ============================================================
# NetLeaf NL 扩展系统综合示例
#
# 本示例演示 include/netleaf_module.h 与 include/netleaf_lang.h 中
# 与“扩展”相关的全部 API，并产出：
#   - netleaf_extension_showcase : 扩展库（.dll / .so），定义 3 个示例扩展
#   - extension_showcase         : 宿主演示程序（可执行文件，带 PASS/FAIL 自检）
#
# 本示例已从 NetLeaf 主构建中解耦：主工程不再通过 add_subdirectory 引入它。
# 它既可被任意父工程以 add_subdirectory 引入（父工程需已定义 netleaf_core），
# 也可以作为一个独立工程单独配置构建：
#
#   cmake -S examples/extension_showcase -B build_showcase \
#         -DNETLEAF_ROOT=<netleaf 源码根目录>      # 先构建好 NetLeaf 主库
#   cmake --build build_showcase
#
# 依赖：NetLeaf 主库 netleaf（核心）与 netleaf_lang（lang 扩展库）。
#       本示例使用 lang 错误码与变量 API，因此必须同时链接 netleaf_lang。
# ============================================================

cmake_minimum_required(VERSION 3.14)

# 独立构建入口：直接对本目录执行 cmake 时进入独立工程模式
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(netleaf_extension_showcase LANGUAGES C)
    set(NETLEAF_SHOWCASE_STANDALONE ON)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(SHOWCASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ------------------------------------------------------------
# 1) 定位 NetLeaf（主库 netleaf + lang 扩展库 netleaf_lang）
#
#    模式 A：被父工程 add_subdirectory 引入，直接用已有的 target。
#    模式 B：独立构建，按 NETLEAF_ROOT 查找已构建的库与头文件。
# ------------------------------------------------------------
if(TARGET netleaf_core)
    # 模式 A：父工程已定义 netleaf_core，头文件位于源码树的 include/
    get_filename_component(NETLEAF_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include" ABSOLUTE)
else()
    # 模式 B：独立构建
    set(NETLEAF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
        "NetLeaf 源码根目录（应包含 include/ 目录以及已构建的 build/ 目录）")

    find_path(NETLEAF_INCLUDE_DIR
        NAMES netleaf.h netleaf_module.h netleaf_lang.h
        HINTS "${NETLEAF_ROOT}/include"
        NO_DEFAULT_PATH)
    if(NOT NETLEAF_INCLUDE_DIR)
        find_path(NETLEAF_INCLUDE_DIR NAMES netleaf.h)
    endif()
    if(NOT NETLEAF_INCLUDE_DIR)
        message(FATAL_ERROR
            "未找到 NetLeaf 头文件 (netleaf.h)。"
            "请用 -DNETLEAF_ROOT=<netleaf 源码根目录> 指定 NetLeaf 位置。")
    endif()

    find_library(NETLEAF_CORE_LIBRARY
        NAMES netleaf libnetleaf
        HINTS "${NETLEAF_ROOT}/build/lib" "${NETLEAF_ROOT}/build/lib/release"
              "${NETLEAF_ROOT}/build/lib/debug" "${NETLEAF_ROOT}/build/bin"
              "${NETLEAF_ROOT}/lib")
    if(NOT NETLEAF_CORE_LIBRARY)
        message(FATAL_ERROR
            "未找到 NetLeaf 主库 netleaf。请先构建 NetLeaf，"
            "或用 -DNETLEAF_ROOT=<根目录> / -DNETLEAF_CORE_LIBRARY=<库文件> 指定。")
    endif()

    add_library(netleaf_core UNKNOWN IMPORTED)
    set_target_properties(netleaf_core PROPERTIES
        IMPORTED_LOCATION "${NETLEAF_CORE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${NETLEAF_INCLUDE_DIR}")
endif()

# lang 扩展库：本示例用到 lang 错误码与变量 API，缺失则无法链接
if(NOT TARGET netleaf_lang)
    find_library(NETLEAF_LANG_LIBRARY
        NAMES netleaf_lang libnetleaf_lang
        HINTS "${NETLEAF_ROOT}/build/lib" "${NETLEAF_ROOT}/build/lib/release"
              "${NETLEAF_ROOT}/build/lib/debug" "${NETLEAF_ROOT}/build/bin"
              "${NETLEAF_ROOT}/lib")
    if(NOT NETLEAF_LANG_LIBRARY)
        message(FATAL_ERROR
            "未找到 lang 扩展库 netleaf_lang。本示例演示 lang 错误码与变量 API，"
            "必须同时构建并链接 netleaf_lang。请先构建 NetLeaf（含 lang 扩展）。")
    endif()
    add_library(netleaf_lang UNKNOWN IMPORTED)
    set_target_properties(netleaf_lang PROPERTIES
        IMPORTED_LOCATION "${NETLEAF_LANG_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${NETLEAF_INCLUDE_DIR}")
endif()

# ------------------------------------------------------------
# 2) 扩展库：定义并导出 3 个示例扩展
# ------------------------------------------------------------
add_library(netleaf_extension_showcase SHARED
    "${SHOWCASE_DIR}/showcase_extension.c"
)

target_include_directories(netleaf_extension_showcase PUBLIC
    "${NETLEAF_INCLUDE_DIR}"
    "${SHOWCASE_DIR}"
)

# NL_EXTENSION_EXPORTS 让 NL_EXT_API 展开为 __declspec(dllexport)
target_compile_definitions(netleaf_extension_showcase PRIVATE
    NL_EXTENSION_EXPORTS
)

set_target_properties(netleaf_extension_showcase PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "netleaf_extension_showcase"
)

target_link_libraries(netleaf_extension_showcase PRIVATE netleaf_core)

# ------------------------------------------------------------
# 3) 宿主演示程序
# ------------------------------------------------------------
add_executable(extension_showcase
    "${SHOWCASE_DIR}/extension_showcase_demo.c"
)

target_include_directories(extension_showcase PRIVATE
    "${NETLEAF_INCLUDE_DIR}"
    "${SHOWCASE_DIR}"
)

target_link_libraries(extension_showcase PRIVATE
    netleaf_core
    netleaf_lang
    netleaf_extension_showcase
)

message(STATUS "NL 扩展综合示例已启用: netleaf_extension_showcase + extension_showcase")
