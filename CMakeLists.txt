# NetLeaf - CMake Build Configuration (Pure C)
# NL扩展系统 v2.4.2
# 架构: 核心库 + 独立扩展，运行时自动加载
# 说明: 各扩展模块的目标定义已拆分到 src/<模块>/CMakeLists.txt，
#       本文件仅负责核心库、模块装配（add_subdirectory）以及示例与测试。

cmake_minimum_required(VERSION 3.14)
project(netleaf VERSION 2.4.2 DESCRIPTION "NetLeaf - A Modern High-Performance Network Library" HOMEPAGE_URL "https://github.com/508364/netleaf" LANGUAGES C)

# Allow overriding the C compiler via CC environment variable or cmake -DCMAKE_C_COMPILER=...
# This supports LLVM-MinGW (clang.exe), MSVC (cl.exe), and any other toolchain.
if(NOT CMAKE_C_COMPILER)
    if(DEFINED ENV{CC})
        set(CMAKE_C_COMPILER "$ENV{CC}" CACHE STRING "C compiler" FORCE)
    endif()
endif()

set(NETLEAF_VERSION_STRING "2.4.2-00000000")
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# The bundled mbedTLS 2.28 declares cmake_minimum_required(VERSION 2.8.12).
# Raise the effective policy floor to 3.10 so modern CMake neither errors out
# (CMake >= 4.0 rejects < 3.5) nor emits the "< 3.10 will be removed"
# deprecation warning for this vendored subproject.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10 CACHE STRING "CMake policy version floor for legacy subprojects" FORCE)

# ============================================================
# Build Options
# ============================================================

option(BUILD_TESTS "Build test programs" OFF)
option(BUILD_EXAMPLES "Build example programs" OFF)
option(BUILD_MQTT "Build MQTT module" ON)
option(BUILD_MQTT_SERVER "Build MQTT server module" ON)
option(BUILD_TLS "Build TLS2 extension (mbedTLS 2.28, TLS 1.0-1.3)" OFF)
option(BUILD_TLS3 "Build TLS3 extension (mbedTLS 3.x, TLS 1.2/1.3)" ON)
option(BUILD_QUICTLS "Build QUIC-TLS extension (self-implemented RFC 9001, requires BUILD_TLS3)" OFF)
option(BUILD_LANG "Build lang extension (official soft dependency)" ON)

# ============================================================
# BUG-303：Link-Time Optimization (LTO)
# ============================================================
# 默认 OFF（保持向后兼容，避免用户需手动重新生成 build 缓存）。
# 启用后：
#   - GCC/Clang：编译加 -flto，链接加 -flto
#   - MSVC：编译 /GL，链接 /LTCG
# 效果：UDP/HTTP 热路径跨文件内联，实测 5-10% 吞吐提升；
#      代价是编译时间 + 二进制体积增加（对体积敏感可关）。
# 注意：不额外提升优化等级（不追加 -O3 / /O2 /Ob3），沿用 CMake 各
#       build type 的默认优化等级，避免对下游用户产生非预期的等级抬升。
option(NETLEAF_ENABLE_LTO "Enable Link-Time Optimization" OFF)

if(NETLEAF_ENABLE_LTO)
    if(MSVC)
        # /GL：生成 LTO 元数据；/LTCG：链接时优化
        add_compile_options(/GL)
        add_link_options(/LTCG)
    else()
        # GCC / Clang：编译 + 链接均加 -flto（不改变默认优化等级）
        add_compile_options(-flto)
        add_link_options(-flto)
        # 对 MinGW 额外补 -fno-fat-lto-objects 以减小 dll 体积
        if(MINGW)
            add_compile_options(-fno-fat-lto-objects)
            add_link_options(-fno-fat-lto-objects)
        endif()
    endif()
    message(STATUS "NetLeaf: LTO enabled (NETLEAF_ENABLE_LTO=ON)")
endif()

# ============================================================
# Threading
# ============================================================

# POSIX 平台的核心库与部分扩展使用 pthread(互斥量 / pthread_once)。
# 统一在此发现线程库；THREADS_PREFER_PTHREAD_FLAG=ON 使 Threads::Threads
# 以 -pthread 方式提供（既设置编译期特性宏，也完成链接），避免仅 -lpthread
# 时共享库未解析符号导致下游可执行文件链接失败。
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ============================================================
# Output Directories
# ============================================================

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(WIN32)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/lib/debug)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/lib/debug)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/lib/release)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/lib/release)
endif()

# ============================================================
# Compiler Flags
# ============================================================

add_compile_options(-Wall -Wextra -Wpedantic)

if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    add_compile_options(-mmacosx-version-min=10.15)
elseif(ANDROID)
    # Android (Bionic): reuse the Linux/epoll-style POSIX sources.
    # -D_FORTIFY_SOURCE 需要优化等级，而 Bionic 不保证在 -O0 下提供
    # __builtin_object_size，故此处不加；保留默认的 -Wall -Wextra -Wpedantic。
    # Android 共享库导出符号通过 NDK 的 __visibility 机制（NL_API 宏）完成，
    # 无需 -z,relro,-z,now。
elseif(UNIX AND NOT APPLE)
    add_compile_options(-D_FORTIFY_SOURCE=2)
    add_link_options(-Wl,-z,relro,-z,now)
endif()

# ============================================================
# Platform-Specific Source Files (Core)
# ============================================================

if(WIN32)
    set(NETLEAF_PLATFORM_SOURCES
        src/windows/netleaf_windows.c
        src/windows/netleaf_encoding_windows.c
        src/windows/netleaf_lazy_windows.c
        src/windows/netleaf_sysinfo_windows.c
    )
    set(NL_OPTIMIZE_HTTP_SOURCES
        src/http/netleaf_http_common.c
        src/http/netleaf_http_shortnames.c
        src/windows/optimize/netleaf_http_windows.c
    )
    set(NL_OPTIMIZE_HTTP_TLS_SOURCES
        src/http/netleaf_http_tls_windows.c
    )
    set(NL_OPTIMIZE_COMMON_SOURCES
        src/windows/optimize/netleaf_optimize_windows.c
        src/windows/optimize/netleaf_websocket_windows.c
    )
elseif(APPLE)
    set(NETLEAF_PLATFORM_SOURCES
        src/macos/netleaf_macos.c
        src/macos/netleaf_encoding_macos.c
        src/macos/netleaf_lazy_macos.c
        src/macos/netleaf_sysinfo_macos.c
    )
    set(NL_OPTIMIZE_HTTP_SOURCES
        src/http/netleaf_http_common.c
        src/http/netleaf_http_shortnames.c
        src/macos/optimize/netleaf_http_macos.c
    )
    set(NL_OPTIMIZE_HTTP_TLS_SOURCES
        src/http/netleaf_http_tls_macos.c
    )
    set(NL_OPTIMIZE_COMMON_SOURCES
        src/macos/optimize/netleaf_optimize_macos.c
        src/macos/optimize/netleaf_websocket_macos.c
    )
elseif(ANDROID)
    # Android 复用 Linux/epoll 风格的 POSIX 源（Bionic 提供 epoll/poll/socket）。
    # Android 没有 /proc/cpuinfo 的 GLIBC 版本概念，sysinfo 源已在 Bionic 下
    # 做了运行时版本识别（见 netleaf_sysinfo_linux.c 的 __ANDROID__ 分支）。
    set(NETLEAF_PLATFORM_SOURCES
        src/linux/netleaf_linux.c
        src/linux/netleaf_encoding_linux.c
        src/linux/netleaf_lazy_linux.c
        src/linux/netleaf_sysinfo_linux.c
    )
    set(NL_OPTIMIZE_HTTP_SOURCES
        src/http/netleaf_http_common.c
        src/http/netleaf_http_shortnames.c
        src/linux/optimize/netleaf_http_linux.c
    )
    set(NL_OPTIMIZE_HTTP_TLS_SOURCES
        src/http/netleaf_http_tls_linux.c
    )
    set(NL_OPTIMIZE_COMMON_SOURCES
        src/linux/optimize/netleaf_optimize_linux.c
        src/linux/optimize/netleaf_websocket_linux.c
    )
elseif(UNIX)
    set(NETLEAF_PLATFORM_SOURCES
        src/linux/netleaf_linux.c
        src/linux/netleaf_encoding_linux.c
        src/linux/netleaf_lazy_linux.c
        src/linux/netleaf_sysinfo_linux.c
    )
    set(NL_OPTIMIZE_HTTP_SOURCES
        src/http/netleaf_http_common.c
        src/http/netleaf_http_shortnames.c
        src/linux/optimize/netleaf_http_linux.c
    )
    set(NL_OPTIMIZE_HTTP_TLS_SOURCES
        src/http/netleaf_http_tls_linux.c
    )
    set(NL_OPTIMIZE_COMMON_SOURCES
        src/linux/optimize/netleaf_optimize_linux.c
        src/linux/optimize/netleaf_websocket_linux.c
    )
endif()

# ============================================================
# Core Library (netleaf) - Pure Networking, No Built-in Modules
# ============================================================

# HTTP（H1/H2/H3 + QUIC 纯逻辑）已融回核心库：netleaf_http_common.c 与各平台
# netleaf_http_<plat>.c 直接编入 netleaf_core，纯 HTTP API 由核心承载。
# TLS/HTTPS 作为"HTTP→TLS 连接的中间层扩展"独立于 src/http/netleaf_http_tls_<plat>.c，
# 由独立 CMake 目标 netleaf_https 编译，安装后注册 HTTPS 相关接口。
# 公共工具层（平台无关）：统一收敛各扩展重复的字符串/哈希/一次性守卫工具，
# 编入核心库并以 NL_API 导出，供所有扩展（链接 netleaf_core）直接调用。
set(NETLEAF_COMMON_SOURCES
    src/common/nl_util.c
)

set(NETLEAF_CORE_SOURCES
    src/netleaf_module.c
    ${NETLEAF_PLATFORM_SOURCES}
    ${NETLEAF_COMMON_SOURCES}
    ${NL_OPTIMIZE_COMMON_SOURCES}
    ${NL_OPTIMIZE_HTTP_SOURCES}
)

add_library(netleaf_core SHARED ${NETLEAF_CORE_SOURCES})
set_target_properties(netleaf_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME "netleaf"
)
target_include_directories(netleaf_core PUBLIC include include/optimize src)
target_compile_definitions(netleaf_core PRIVATE NL_EXPORTS)

# Platform-specific linking for core
if(WIN32)
    target_link_libraries(netleaf_core PRIVATE ws2_32 winmm mswsock bcrypt)
elseif(APPLE)
    target_link_libraries(netleaf_core PRIVATE Threads::Threads iconv)
elseif(ANDROID)
    # Bionic: dl/dlsym/dlopen 已并入 libc，无独立 -ldl；iconv 同样内置。
    # 仅显式链接 pthread（NDK 实际并入 libc，但保留以维持可移植性）与 log
    # （Bionic 的 __android_log 由 liblog 提供，netleaf 可能调用）。
    target_link_libraries(netleaf_core PRIVATE Threads::Threads log)
elseif(UNIX)
    target_link_libraries(netleaf_core PRIVATE Threads::Threads dl)
endif()

# ============================================================
# Extensions - 各扩展模块的目标定义位于各自的 src/<模块>/CMakeLists.txt
# ============================================================

# 基础扩展（始终参与构建，运行时由宿主按需加载）
# Lang 为官方软依赖：BUILD_LANG=ON 时构建并定义 NL_LANG_ENABLE；OFF 时扩展
# 不链接 lang，netleaf_lang.h 中的声明/宏退化为 no-op，无需改动扩展源码。
if(BUILD_LANG)
    add_subdirectory(src/lang)
endif()
add_subdirectory(src/ipc)
add_subdirectory(src/linkagg)
add_subdirectory(src/vue)
add_subdirectory(src/autocomplete)
add_subdirectory(src/autoroute)
add_subdirectory(src/errorpage)

# TLS 扩展（可选，含内置 mbedTLS 2.28 集成）
# 注意：mbedTLS 2.28 与 3.x 各自以 add_subdirectory 引入，二者都会创建
# CMake 目标 mbedtls/mbedx509/mbedcrypto，同一构建内不允许两个同名目标，
# 故 BUILD_TLS 与 BUILD_TLS3 在同一构建中互斥（源码层面 tls2/tls3 两库可共存）。
if(BUILD_TLS AND BUILD_TLS3)
    message(WARNING "BUILD_TLS 与 BUILD_TLS3 在同一构建中互斥，已忽略 BUILD_TLS。"
                    "请仅保留其一；若需两库共存，请分别构建。")
    set(BUILD_TLS OFF)
endif()

if(BUILD_TLS)
    add_subdirectory(src/tls)
endif()

# TLS3 扩展（可选，内置 mbedTLS 3.x，支持 TLS 1.2/1.3）
# 当 BUILD_TLS 与 BUILD_TLS3 同时启用时，netleaf_https 优先链接 tls3。
if(BUILD_TLS3)
    add_subdirectory(src/tls3)
endif()

# QUIC-TLS 扩展（自研 RFC 9001 数据包保护层，复用 tls3 引入的 mbedTLS 目标）
if(BUILD_QUICTLS)
    if(BUILD_TLS3)
        add_subdirectory(src/quictls)
    else()
        message(WARNING "BUILD_QUICTLS=ON 需要 BUILD_TLS3=ON（复用其 mbedTLS 目标），已跳过 quictls。")
    endif()
endif()

# HTTPS 扩展库（TLS/HTTPS：HTTP→TLS 连接的中间层）：
# 纯 HTTP API 已由核心库承载，本目标仅编译 netleaf_http_tls_<plat>.c，
# 链接 netleaf_core（获取 nl_http_server_t 等 HTTP 类型）+
# netleaf_tls3/tls2 + mbedtls。
# 该库导出 nl_https_get_extension_info，安装/加载后注册 HTTPS 相关接口
# （nl_http_server_enable_tls / nl_http_client_*），见 src/netleaf_module.c
# 的 g_ext_symbol_patterns 自动发现白名单。
# BUILD_TLS=OFF 时不构建；BUILD_TLS3 时优先链接 tls3，否则链接 tls2。
if(BUILD_TLS OR BUILD_TLS3)
    # ---- HTTPS（TLS）扩展库 ----
    add_library(netleaf_https SHARED
        ${NL_OPTIMIZE_HTTP_TLS_SOURCES}
        src/http/netleaf_https_ext.c
    )
    target_include_directories(netleaf_https PUBLIC
        include
        include/optimize
        src
    )
    target_include_directories(netleaf_https PRIVATE
        src/http
    )
    target_compile_definitions(netleaf_https PRIVATE NL_HTTPS_ENABLE NL_EXPORTS NL_EXTENSION_EXPORTS)
    # 共存时优先 tls3；仅 tls2 时用 tls2。
    if(BUILD_TLS3)
        target_compile_definitions(netleaf_https PRIVATE NL_HTTPS_USE_TLS3)
        target_link_libraries(netleaf_https PRIVATE
            netleaf_core
            netleaf_tls3
            mbedtls
            mbedx509
            mbedcrypto
        )
    else()
        target_link_libraries(netleaf_https PRIVATE
            netleaf_core
            netleaf_tls2
            mbedtls
            mbedx509
            mbedcrypto
        )
    endif()
    if(WIN32)
        target_link_libraries(netleaf_https PRIVATE ws2_32)
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(netleaf_https PRIVATE pthread)
    endif()
    if(BUILD_LANG)
        target_link_libraries(netleaf_https PRIVATE netleaf_lang)
    endif()
endif()

# MQTT 客户端 / 服务端扩展（可选）
if(BUILD_MQTT OR BUILD_MQTT_SERVER)
    add_subdirectory(src/mqtt)
endif()

# ============================================================
# Install Rules
# ============================================================

# Install core library
install(TARGETS netleaf_core
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# Install HTTPS extension library (HTTP→TLS 中间层扩展)
if(BUILD_TLS OR BUILD_TLS3)
    install(TARGETS netleaf_https
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
    )
endif()

# Install core headers (各扩展模块的自有头文件由其各自的 CMakeLists.txt 安装)
install(DIRECTORY include/netleaf DESTINATION include FILES_MATCHING PATTERN "*.h")
install(FILES
    include/netleaf.h
    include/netleaf_module.h
    include/nl_util.h
    DESTINATION include
)

# ============================================================
# Examples
# ============================================================

if(BUILD_EXAMPLES)
    # Example: basic server
    add_executable(example_server test/test_simple.c)
    target_link_libraries(example_server PRIVATE netleaf_core)

    # Example: extension loading
    add_executable(example_extension examples/plugin_example/test_extension.c)
    target_link_libraries(example_extension PRIVATE netleaf_core ${CMAKE_DL_LIBS})

    # Example: plugin template (build as shared library)
    add_library(example_plugin_template SHARED examples/plugin_template/plugin.c)
    target_link_libraries(example_plugin_template PRIVATE netleaf_core)
    target_compile_definitions(example_plugin_template PRIVATE NL_PLUGIN_EXPORTS)

    # 注意: examples/extension_showcase 已从主构建解耦（源码保留原地，供独立仓库使用），
    #       主工程不再通过 add_subdirectory 引入其任何 target。
endif()

# ============================================================
# Tests
# ============================================================

# 测试程序注册已拆分至 test/CMakeLists.txt（由 BUILD_TESTS 开关控制）。
# MQTT / MQTT Server 开关（BUILD_MQTT / BUILD_MQTT_SERVER）决定是否构建
# netleaf_mqtt / netleaf_mqtt_server 目标；test/CMakeLists.txt 会据此
# 裁剪 test_modules / test_full / test_all 的链接列表与 test_mqtt_server_loopback。
enable_testing()

if(BUILD_TESTS)
    add_subdirectory(test)
endif()

# ============================================================
# Summary
# ============================================================

message(STATUS "")
message(STATUS "NetLeaf ${PROJECT_VERSION} Build Configuration")
message(STATUS "===========================================")
message(STATUS "Core Library:     libnetleaf.dll/.so")
message(STATUS "Extensions:")
message(STATUS "  - libnetleaf_lang.dll/.so        (Language support)")
message(STATUS "  - libnetleaf_ipc.dll/.so         (IPC communication)")
message(STATUS "  - libnetleaf_linkagg.dll/.so     (Link aggregation)")
message(STATUS "  - libnetleaf_autoroute.dll/.so   (Auto route)")
message(STATUS "  - libnetleaf_autocomplete.dll/.so (Auto complete)")
message(STATUS "  - libnetleaf_errorpage.dll/.so   (Error pages)")
message(STATUS "  - libnetleaf_vue.dll/.so         (Vue.js support)")
if(BUILD_TLS)
    message(STATUS "  - libnetleaf_tls2.dll/.so        (TLS/SSL encryption, mbedTLS 2.28)")
endif()
if(BUILD_TLS3)
    message(STATUS "  - libnetleaf_tls3.dll/.so        (TLS 1.2/1.3 encryption, mbedTLS 3.x)")
endif()
if(BUILD_TLS OR BUILD_TLS3)
    message(STATUS "  - libnetleaf_https.dll/.so       (HTTPS/TLS, TLS-gated, extension: registers HTTPS APIs)")
endif()
if(BUILD_MQTT)
    message(STATUS "  - libnetleaf_mqtt.dll/.so        (MQTT v3.1.1/v5.0 client)")
endif()
if(BUILD_MQTT_SERVER)
    message(STATUS "  - libnetleaf_mqtt_server.dll/.so (MQTT v5.0 server)")
endif()
message(STATUS "")
message(STATUS "Architecture: Core + Extensions (runtime loaded)")
message(STATUS "")
