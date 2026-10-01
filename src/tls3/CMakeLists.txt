# TLS3 扩展模块构建脚本
# 内置 mbedTLS 3.x 集成 + TLS/SSL 加密扩展（TLS 1.2/1.3）
# 目标产物：libnetleaf_tls3.dll/.so
# 说明：tls3 基于 mbedTLS 3.x；当 mbedTLS 2.28（tls2）与 3.x（tls3）同时
#       以 add_subdirectory 引入时，二者都会创建 CMake 目标
#       mbedtls / mbedx509 / mbedcrypto，同一构建树中不允许同名目标共存，
#       故 BUILD_TLS 与 BUILD_TLS3 在顶层 CMakeLists.txt 中互斥（顶层已处理）。

message(STATUS "Building tls3 module...")

# 使用 third-party 目录中内置的 mbedTLS 3.x。
# mbedTLS 3.x 已彻底移除 TLS 1.0/1.1，仅支持 TLS 1.2/1.3（及 DTLS 1.2/1.3）。
set(MBEDTLS3_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third-party/mbedtls3"
    CACHE PATH "Path to mbedTLS 3.x source directory")
message(STATUS "TLS3 Extension: Using built-in mbedTLS 3.x from ${MBEDTLS3_SOURCE_DIR}")

# 将 mbedTLS 3.x 作为子目录引入
if(EXISTS "${MBEDTLS3_SOURCE_DIR}/CMakeLists.txt")
    # 仅构建 mbedTLS 静态库，并关闭其测试与示例程序。
    # 注意：mbedTLS 3.x 的目标名称与 2.28 相同（mbedtls / mbedx509 / mbedcrypto），
    # 因此 BUILD_TLS（tls2）与 BUILD_TLS3（tls3）不能在同一构建树中同时启用；
    # 顶层 CMakeLists.txt 已通过 BUILD_TLS/BUILD_TLS3 互斥逻辑处理此情况。
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "Build mbedTLS static libraries" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "Build mbedTLS shared libraries" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "Build Mbed TLS tests" FORCE)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "Build Mbed TLS programs" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "Compiler warnings treated as errors" FORCE)

    # mbedTLS 3.x 以静态库形式被链入 netleaf_tls3 共享库，必须启用 PIC。
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    add_subdirectory("${MBEDTLS3_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/mbedtls3")
else()
    message(FATAL_ERROR
        "mbedTLS 3.x source not found at ${MBEDTLS3_SOURCE_DIR}. "
        "Please download mbedTLS 3.x and place it in third-party/mbedtls3/."
    )
endif()

# TLS3 扩展源文件
add_library(netleaf_tls3 SHARED
    netleaf_tls3.c
)
set_target_properties(netleaf_tls3 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME "netleaf_tls3"
)

target_include_directories(netleaf_tls3 PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/mqtt
    ${MBEDTLS3_SOURCE_DIR}/include
)

target_compile_definitions(netleaf_tls3 PRIVATE
    NL_TLS3_EXPORTS
    NL_TLS3_ENABLE
)

target_link_libraries(netleaf_tls3 PRIVATE
    netleaf_core
    mbedtls
    mbedx509
    mbedcrypto
)

# Lang 为官方软依赖：BUILD_LANG=ON 时链接并定义 NL_LANG_ENABLE
if(BUILD_LANG)
    target_link_libraries(netleaf_tls3 PRIVATE netleaf_lang)
endif()

# 平台相关链接
if(WIN32)
    target_link_libraries(netleaf_tls3 PRIVATE ws2_32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(netleaf_tls3 PRIVATE pthread)
endif()

# 安装规则
install(TARGETS netleaf_tls3
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES
    ${CMAKE_SOURCE_DIR}/include/netleaf_tls3.h
    DESTINATION include/netleaf
)

# 安装 mbedTLS 3.x 头文件
install(DIRECTORY "${MBEDTLS3_SOURCE_DIR}/include/mbedtls"
        DESTINATION include/mbedtls
        FILES_MATCHING PATTERN "*.h")
install(DIRECTORY "${MBEDTLS3_SOURCE_DIR}/include/psa"
        DESTINATION include/psa
        FILES_MATCHING PATTERN "*.h")
