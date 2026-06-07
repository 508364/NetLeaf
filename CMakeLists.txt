cmake_minimum_required(VERSION 3.15)
project(NetLeaf VERSION 2.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# 宽可用库配置选项
option(WIDE_LIB "Build as wide-usable library (simple output structure)" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_STATIC_LIBS "Build static libraries" ON)
option(BUILD_EXAMPLES "Build example programs" OFF)

# 平台检测
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PLATFORM_NAME "Linux")
    set(PLATFORM_SOURCE "linux")
    message(STATUS "Platform: Linux (epoll)")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PLATFORM_NAME "Windows")
    set(PLATFORM_SOURCE "windows")
    message(STATUS "Platform: Windows (IOCP)")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

# 源文件
set(PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/${PLATFORM_SOURCE}")
set(PLATFORM_OPT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/${PLATFORM_SOURCE}/optimize")

file(GLOB PLATFORM_SOURCES "${PLATFORM_DIR}/*.c")
file(GLOB PLATFORM_OPT_SOURCES "${PLATFORM_OPT_DIR}/*.c")

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
    set(LINK_LIBS ws2_32 mswsock advapi32)
endif()

# 编译器优化
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O3")
elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W3 /O2")
endif()

# 共享库
add_library(netleaf_shared SHARED ${PLATFORM_SOURCES} ${PLATFORM_OPT_SOURCES})
target_link_libraries(netleaf_shared ${LINK_LIBS})
set_target_properties(netleaf_shared PROPERTIES
    OUTPUT_NAME "netleaf"
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

# 静态库
if(BUILD_STATIC_LIBS)
    add_library(netleaf_static STATIC ${PLATFORM_SOURCES} ${PLATFORM_OPT_SOURCES})
    target_link_libraries(netleaf_static ${LINK_LIBS})
    set_target_properties(netleaf_static PROPERTIES
        OUTPUT_NAME "netleaf"
    )
    if(NOT PLATFORM_NAME STREQUAL "Windows")
        set_target_properties(netleaf_static PROPERTIES
            COMPILE_FLAGS "-fPIC"
        )
    endif()
endif()

# 构建摘要
message(STATUS "")
message(STATUS "========================================")
message(STATUS "  NetLeaf v${PROJECT_VERSION} - Library Only")
message(STATUS "========================================")
message(STATUS "Platform: ${PLATFORM_NAME}")
if(NOT WIDE_LIB)
    message(STATUS "Architecture: ${ARCH_NAME}")
endif()
message(STATUS "Mode: ${WIDE_LIB} (Wide-Usable)")
message(STATUS "")
message(STATUS "Output directories:")
message(STATUS "  Libraries: ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
message(STATUS "")
