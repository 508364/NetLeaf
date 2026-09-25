# NetLeaf - CMake Build Configuration (Pure C)
# NL扩展系统 v2.4.1
# 架构: 核心库 + 独立扩展，运行时自动加载
# 说明: 各扩展模块的目标定义已拆分到 src/<模块>/CMakeLists.txt，
#       本文件仅负责核心库、模块装配（add_subdirectory）以及示例与测试。

cmake_minimum_required(VERSION 3.14)
project(netleaf VERSION 2.4.1 DESCRIPTION "NetLeaf - A Modern High-Performance Network Library" HOMEPAGE_URL "https://github.com/508364/netleaf" LANGUAGES C)

# Allow overriding the C compiler via CC environment variable or cmake -DCMAKE_C_COMPILER=...
# This supports LLVM-MinGW (clang.exe), MSVC (cl.exe), and any other toolchain.
if(NOT CMAKE_C_COMPILER)
    if(DEFINED ENV{CC})
        set(CMAKE_C_COMPILER "$ENV{CC}" CACHE STRING "C compiler" FORCE)
    endif()
endif()

set(NETLEAF_VERSION_STRING "2.4.1-00000000")
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
option(BUILD_MQTT "Build MQTT module" OFF)
option(BUILD_MQTT_SERVER "Build MQTT server module" OFF)
option(BUILD_TLS "Build TLS/SSL extension (requires mbedTLS)" OFF)

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
elseif(APPLE)
    set(NETLEAF_PLATFORM_SOURCES
        src/macos/netleaf_macos.c
        src/macos/netleaf_encoding_macos.c
        src/macos/netleaf_lazy_macos.c
        src/macos/netleaf_sysinfo_macos.c
    )
elseif(UNIX)
    set(NETLEAF_PLATFORM_SOURCES
        src/linux/netleaf_linux.c
        src/linux/netleaf_encoding_linux.c
        src/linux/netleaf_lazy_linux.c
        src/linux/netleaf_sysinfo_linux.c
    )
endif()

# ============================================================
# Core Library (netleaf) - Pure Networking, No Built-in Modules
# ============================================================

set(NETLEAF_CORE_SOURCES
    src/netleaf_module.c
    ${NETLEAF_PLATFORM_SOURCES}
)

add_library(netleaf_core SHARED ${NETLEAF_CORE_SOURCES})
set_target_properties(netleaf_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME "netleaf"
)
target_include_directories(netleaf_core PUBLIC include src)
target_compile_definitions(netleaf_core PRIVATE NL_EXPORTS)

# Platform-specific linking for core
if(WIN32)
    target_link_libraries(netleaf_core PRIVATE ws2_32 winmm mswsock bcrypt)
elseif(APPLE)
    target_link_libraries(netleaf_core PRIVATE Threads::Threads iconv)
elseif(UNIX)
    target_link_libraries(netleaf_core PRIVATE Threads::Threads dl)
endif()

# ============================================================
# Extensions - 各扩展模块的目标定义位于各自的 src/<模块>/CMakeLists.txt
# ============================================================

# 基础扩展（始终参与构建，运行时由宿主按需加载）
add_subdirectory(src/lang)
add_subdirectory(src/ipc)
add_subdirectory(src/linkagg)
add_subdirectory(src/vue)
add_subdirectory(src/autocomplete)
add_subdirectory(src/autoroute)
add_subdirectory(src/errorpage)

# TLS 扩展（可选，含内置 mbedTLS 集成）
if(BUILD_TLS)
    add_subdirectory(src/tls)
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

# Install core headers (各扩展模块的自有头文件由其各自的 CMakeLists.txt 安装)
install(DIRECTORY include/netleaf DESTINATION include FILES_MATCHING PATTERN "*.h")
install(FILES
    include/netleaf.h
    include/netleaf_module.h
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

enable_testing()

if(BUILD_TESTS)
    # Test modules
    add_executable(test_modules test/test_modules.c)
    target_link_libraries(test_modules PRIVATE netleaf_core netleaf_lang netleaf_ipc netleaf_linkagg netleaf_autocomplete netleaf_autoroute netleaf_errorpage netleaf_vue netleaf_mqtt netleaf_mqtt_server)
    add_test(NAME ModulesTest COMMAND test_modules)

    # Test full features
    add_executable(test_full test/test_full.c)
    target_link_libraries(test_full PRIVATE netleaf_core netleaf_lang netleaf_ipc netleaf_linkagg netleaf_autocomplete netleaf_autoroute netleaf_errorpage netleaf_vue netleaf_mqtt netleaf_mqtt_server)
    add_test(NAME FullTest COMMAND test_full)

    # Test variable substitution (v2.4.0)
    add_executable(test_variable_substitution test/test_variable_substitution.c)
    target_link_libraries(test_variable_substitution PRIVATE netleaf_core netleaf_lang)
    add_test(NAME VariableSubstitutionTest COMMAND test_variable_substitution)

    # Test all modules comprehensive
    add_executable(test_all test/test_all.c)
    target_link_libraries(test_all PRIVATE netleaf_core netleaf_lang netleaf_ipc netleaf_linkagg netleaf_autocomplete netleaf_autoroute netleaf_errorpage netleaf_vue netleaf_mqtt netleaf_mqtt_server)
    target_compile_definitions(test_all PRIVATE
        $<$<BOOL:${BUILD_MQTT}>:HAVE_NETLEAF_MQTT>
        $<$<BOOL:${BUILD_MQTT_SERVER}>:HAVE_NETLEAF_MQTT_SERVER>
    )
    add_test(NAME AllModulesTest COMMAND test_all)

    # MQTT 帧编码 / 缓冲区扩容回归测试（直接包含实现，不依赖真实网络与 mqtt 目标）
    add_executable(test_mqtt_frames test/test_mqtt_frames.c)
    target_link_libraries(test_mqtt_frames PRIVATE netleaf_core netleaf_lang)
    if(WIN32)
        # 该测试直接包含 netleaf_mqtt.c，会用到 winsock 等系统库
        target_link_libraries(test_mqtt_frames PRIVATE ws2_32 winmm mswsock bcrypt)
    endif()
    target_include_directories(test_mqtt_frames PRIVATE ${CMAKE_SOURCE_DIR}/src/mqtt)
    target_compile_definitions(test_mqtt_frames PRIVATE NL_MQTT_STATIC)
    add_test(NAME MqttFramesTest COMMAND test_mqtt_frames)

    # MQTT 服务端事件循环 / 真实下发 集成测试（回环 socket，需 mqtt + mqtt_server 目标）
    if(BUILD_MQTT AND BUILD_MQTT_SERVER)
        add_executable(test_mqtt_server_loopback test/test_mqtt_server_loopback.c)
        target_link_libraries(test_mqtt_server_loopback PRIVATE
            netleaf_core netleaf_lang netleaf_mqtt netleaf_mqtt_server)
        if(WIN32)
            target_link_libraries(test_mqtt_server_loopback PRIVATE ws2_32 winmm mswsock bcrypt)
        endif()
        target_include_directories(test_mqtt_server_loopback PRIVATE ${CMAKE_SOURCE_DIR}/include)
        add_test(NAME MqttServerLoopbackTest COMMAND test_mqtt_server_loopback)
    endif()
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
    message(STATUS "  - libnetleaf_tls.dll/.so         (TLS/SSL encryption)")
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
