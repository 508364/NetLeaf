# QUIC-TLS 扩展模块构建脚本
# 自研 QUIC-TLS（RFC 9001）数据包保护层，基于 mbedTLS 原语（HMAC/AES-GCM/AES-ECB）。
# 目标产物：libnetleaf_quictls.dll/.so
# 依赖：复用 src/tls3 引入的 mbedTLS 3.x 目标（mbedtls/mbedx509/mbedcrypto），
#       故 BUILD_QUICTLS 需 BUILD_TLS3=ON（顶层已做保护）。

message(STATUS "Building quictls module...")

add_library(netleaf_quictls SHARED
    netleaf_quictls.c
    quictls_tls.c
    quictls_handshake.c
    quictls_quic.c
    quictls_recovery.c
    quictls_flowcontrol.c
    quictls_qpack.c
    quictls_huffman.c
    quictls_qpack_stream.c
    quictls_qpack_link.c
)
set_target_properties(netleaf_quictls PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME "netleaf_quictls"
)

target_include_directories(netleaf_quictls PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third-party/mbedtls3/include
)

target_compile_definitions(netleaf_quictls PRIVATE
    NL_QUICTLS_EXPORTS
)

target_link_libraries(netleaf_quictls PRIVATE
    netleaf_core
    mbedtls
    mbedx509
    mbedcrypto
)

if(WIN32)
    target_link_libraries(netleaf_quictls PRIVATE ws2_32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(netleaf_quictls PRIVATE pthread)
endif()

install(TARGETS netleaf_quictls
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES
    ${CMAKE_SOURCE_DIR}/include/netleaf_quictls.h
    DESTINATION include/netleaf
)
