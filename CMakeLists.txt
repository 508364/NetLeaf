cmake_minimum_required(VERSION 3.15)
project(NetLeaf VERSION 2.2.2 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# 宽可用库配置选项
option(WIDE_LIB "Build as wide-usable library (simple output structure)" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_STATIC_LIBS "Build static libraries" ON)
option(BUILD_EXAMPLES "Build example programs" OFF)
option(BUILD_AUTOCOMPLETE "Build autocomplete module (auto charset/Vue completion)" ON)
option(BUILD_AUTOROUTE "Build autoroute module (auto route suggestions)" ON)
option(BUILD_ERRORPAGE "Build errorpage module (better error pages with templates)" ON)
option(BUILD_IPC "Build IPC communication module (Windows/Linux only)" ON)
option(BUILD_LINKAGG "Build link aggregation module (Windows/Linux only)" ON)
option(BUILD_VUE "Build Vue module (Vue.js backend support)" ON)
option(BUILD_ASAN "Build with AddressSanitizer (Debug only)" OFF)

# 平台检测
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PLATFORM_NAME "Linux")
    set(PLATFORM_SOURCE "linux")
    message(STATUS "Platform: Linux (epoll)")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PLATFORM_NAME "Windows")
    set(PLATFORM_SOURCE "windows")
    message(STATUS "Platform: Windows (IOCP)")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PLATFORM_NAME "macOS")
    set(PLATFORM_SOURCE "macos")
    message(STATUS "Platform: macOS (kqueue)")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

# 源文件
set(PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/${PLATFORM_SOURCE}")
set(PLATFORM_OPT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/${PLATFORM_SOURCE}/optimize")

file(GLOB PLATFORM_SOURCES "${PLATFORM_DIR}/*.c")
file(GLOB PLATFORM_OPT_SOURCES "${PLATFORM_OPT_DIR}/*.c")

# 添加模块管理源文件
list(APPEND PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/netleaf_module.c")

# 排除独立模块的平台源文件（它们由各自的 CMakeLists.txt 管理）
# 注意：Windows 文件名需要转义反斜杠
if(PLATFORM_NAME STREQUAL "Windows")
    set(EXCLUDED_FILES "netleaf_ipc_windows.c" "netleaf_linkagg_windows.c")
elseif(PLATFORM_NAME STREQUAL "Linux")
    set(EXCLUDED_FILES "netleaf_ipc_linux.c" "netleaf_linkagg_linux.c")
else()
    set(EXCLUDED_FILES "")
endif()

foreach(EXCLUDED_FILE ${EXCLUDED_FILES})
    list(FILTER PLATFORM_SOURCES EXCLUDE REGEX "${EXCLUDED_FILE}$")
    list(FILTER PLATFORM_OPT_SOURCES EXCLUDE REGEX "${EXCLUDED_FILE}$")
endforeach()

# 头文件目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/optimize
)

# 宽可用库模式 - 简单统一的输出结构
if(WIDE_LIB)
    message(STATUS "Mode: Wide-Usable Library")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
else()
    message(STATUS "Mode: Architecture-Specific")
    # 检测架构
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64|x64")
        set(ARCH_NAME "x64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
        set(ARCH_NAME "arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM|arm")
        set(ARCH_NAME "arm")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "X86|x86|i686|i386")
        set(ARCH_NAME "x86")
    else()
        set(ARCH_NAME "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/${PLATFORM_NAME}/${ARCH_NAME}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib/${PLATFORM_NAME}/${ARCH_NAME}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib/${PLATFORM_NAME}/${ARCH_NAME}")
endif()

# 平台特定配置
if(PLATFORM_NAME STREQUAL "Linux")
    set(SHARED_LIB_EXT ".so")
    set(STATIC_LIB_EXT ".a")
    find_package(Threads REQUIRED)
    set(LINK_LIBS ${CMAKE_THREAD_LIBS_INIT} m)
    add_definitions(-DNL_PLATFORM_LINUX)
elseif(PLATFORM_NAME STREQUAL "Windows")
    set(SHARED_LIB_EXT ".dll")
    set(STATIC_LIB_EXT ".lib")
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-D_WINSOCK_DEPRECATED_NO_WARNINGS)
    add_definitions(-DNL_PLATFORM_WINDOWS)
    if(BUILD_SHARED_LIBS)
        add_definitions(-DNL_EXPORTS)
    endif()
    if(MSVC)
        add_compile_options(/utf-8)
    endif()
    set(LINK_LIBS ws2_32 mswsock advapi32 bcrypt)
elseif(PLATFORM_NAME STREQUAL "macOS")
    set(SHARED_LIB_EXT ".dylib")
    set(STATIC_LIB_EXT ".a")
    find_package(Threads REQUIRED)
    find_library(ICONV_LIB iconv)
    set(LINK_LIBS ${CMAKE_THREAD_LIBS_INIT} m ${ICONV_LIB})
    add_definitions(-DNL_PLATFORM_MACOS)
    # macOS specific
    if(BUILD_SHARED_LIBS)
        add_definitions(-DNL_EXPORTS)
    endif()
endif()

# ASan support (GCC/Clang only)
if(BUILD_ASAN)
    if(MSVC)
        message(WARNING "AddressSanitizer is not supported with MSVC. Ignoring BUILD_ASAN.")
    else()
        message(STATUS "Building with AddressSanitizer enabled")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
    endif()
endif()

# 编译器优化
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O3 -Wno-stringop-truncation -Wno-stringop-overflow -Wno-format-truncation -Wno-unused-result")
    # LTO (Link Time Optimization) - Linux only
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -flto")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -flto")
    endif()
    # Security hardening flags
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fstack-protector-strong -D_FORTIFY_SOURCE=2")
    # Linux-specific linker flags (not for macOS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie -Wl,-z,relro,-z,now")
    endif()
elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O3 -Wno-string-conversion")
    # LTO (Link Time Optimization) - Linux only, macOS osxcross may have issues
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -flto")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -flto")
    endif()
    # Security hardening flags - Linux only
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fstack-protector-strong -D_FORTIFY_SOURCE=2")
        # Linux-specific linker flags
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie -Wl,-z,relro,-z,now")
    endif()
elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W3 /O2")
    # LTO (Link Time Optimization) for MSVC
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /GL")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /LTCG")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /LTCG")
    # Security hardening flags for MSVC
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /GS /sdl /DYNAMICBASE")
endif()

# 共享库
add_library(netleaf_shared SHARED ${PLATFORM_SOURCES} ${PLATFORM_OPT_SOURCES})
target_link_libraries(netleaf_shared PUBLIC ${LINK_LIBS})
if(PLATFORM_NAME STREQUAL "macOS")
    set_target_properties(netleaf_shared PROPERTIES
        INSTALL_RPATH "@loader_path"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()
set_target_properties(netleaf_shared PROPERTIES
    OUTPUT_NAME "netleaf"
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

# 静态库
if(BUILD_STATIC_LIBS)
    add_library(netleaf_static STATIC ${PLATFORM_SOURCES} ${PLATFORM_OPT_SOURCES})
    target_link_libraries(netleaf_static 
        ${LINK_LIBS}
        netleaf_lang
    )
    set_target_properties(netleaf_static PROPERTIES
        OUTPUT_NAME "netleaf"
    )
    if(NOT PLATFORM_NAME STREQUAL "Windows")
        set_target_properties(netleaf_static PROPERTIES
            COMPILE_FLAGS "-fPIC"
        )
    endif()
endif()

# 示例程序
if(BUILD_EXAMPLES)
    message(STATUS "Building examples...")
    
    set(EXAMPLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    
    add_executable(example_all_features ${EXAMPLES_DIR}/example_all_features.c)
    target_link_libraries(example_all_features 
        netleaf_static 
        netleaf_autocomplete
        netleaf_autoroute
        netleaf_errorpage
        ${LINK_LIBS}
    )
    target_compile_definitions(example_all_features PRIVATE DEBUG_LOGGING)
    
    message(STATUS "Examples will be built to: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

# 可选模块 - Auto-complete
if(BUILD_AUTOCOMPLETE)
    message(STATUS "Building autocomplete module...")
    add_subdirectory(src/autocomplete)
endif()

# 可选模块 - Auto-route
if(BUILD_AUTOROUTE)
    message(STATUS "Building autoroute module...")
    add_subdirectory(src/autoroute)
endif()

# 必需模块 - Language (multi-language error messages)
message(STATUS "Building lang module...")
message(STATUS "  - Lang v${PROJECT_VERSION} (multi-language error messages)")
add_subdirectory(src/lang)

# 可选模块 - Better Error Page
if(BUILD_ERRORPAGE)
    message(STATUS "Building errorpage module...")
    add_subdirectory(src/errorpage)
endif()

# 可选模块 - IPC (Windows/Linux only)
if(BUILD_IPC)
    message(STATUS "Building ipc module...")
    add_subdirectory(src/ipc)
endif()

# 可选模块 - Link Aggregation (Windows/Linux only)
if(BUILD_LINKAGG)
    message(STATUS "Building linkagg module...")
    add_subdirectory(src/linkagg)
endif()

# 可选模块 - Vue (Vue.js backend support)
if(BUILD_VUE)
    message(STATUS "Building vue module...")
    add_subdirectory(src/vue)
endif()

# 添加测试程序

# 构建摘要
message(STATUS "")
message(STATUS "========================================")
message(STATUS "  NetLeaf v${PROJECT_VERSION}")
message(STATUS "========================================")
message(STATUS "Platform: ${PLATFORM_NAME}")
if(NOT WIDE_LIB)
    message(STATUS "Architecture: ${ARCH_NAME}")
endif()
message(STATUS "Mode: ${WIDE_LIB} (Wide-Usable)")
message(STATUS "Build lang: ON")
message(STATUS "Build examples: ${BUILD_EXAMPLES}")
message(STATUS "Build autocomplete: ${BUILD_AUTOCOMPLETE}")
message(STATUS "Build autoroute: ${BUILD_AUTOROUTE}")
message(STATUS "Build errorpage: ${BUILD_ERRORPAGE}")
message(STATUS "Build ipc: ${BUILD_IPC}")
message(STATUS "Build linkagg: ${BUILD_LINKAGG}")
message(STATUS "Build vue: ${BUILD_VUE}")
message(STATUS "")
message(STATUS "Output directories:")
message(STATUS "  Libraries: ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
if(BUILD_EXAMPLES)
    message(STATUS "  Examples: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()
message(STATUS "")
