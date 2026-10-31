# QUIC 扩展模块构建脚本（原 quictls 扩展，与主库迁出的 H3/QUIC 纯逻辑合并）
# 含自研 QUIC-TLS（RFC 9001）数据包保护层，基于 mbedTLS 原语（HMAC/AES-GCM/AES-ECB），
# 以及原主库 H3/QUIC 纯逻辑（quic_h3_legacy.c，零密钥演示态，暂不再开发）。
# 目标产物：libnetleaf_quic.dll/.so
# 依赖：复用 src/tls3 引入的 mbedTLS 3.x 目标（mbedtls/mbedx509/mbedcrypto），
#       故 BUILD_QUIC 需 BUILD_TLS3=ON（顶层已做保护）。

message(STATUS "Building quic module...")

add_library(netleaf_quic SHARED
    netleaf_quic.c
    quictls_tls.c
    quictls_handshake.c
    quictls_quic.c
    quictls_recovery.c
    quictls_flowcontrol.c
    quictls_qpack.c
    quictls_huffman.c
    quictls_qpack_stream.c
    quictls_qpack_link.c
    quic_h3_legacy.c
)
set_target_properties(netleaf_quic PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME "netleaf_quic"
)

target_include_directories(netleaf_quic PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third-party/mbedtls3/include
)

target_compile_definitions(netleaf_quic PRIVATE
    NL_QUICTLS_EXPORTS
    NLH3_DBG
)

target_link_libraries(netleaf_quic PRIVATE
    netleaf_core
    mbedtls
    mbedx509
    mbedcrypto
)

if(WIN32)
    target_link_libraries(netleaf_quic PRIVATE ws2_32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(netleaf_quic PRIVATE pthread)
endif()

install(TARGETS netleaf_quic
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES
    ${CMAKE_SOURCE_DIR}/include/netleaf_quic.h
    DESTINATION include/netleaf
)
