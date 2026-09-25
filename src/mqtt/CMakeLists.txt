# MQTT 扩展模块构建脚本
# 包含 MQTT 客户端（netleaf_mqtt）与 MQTT 服务端（netleaf_mqtt_server）两个目标
# 目标产物：libnetleaf_mqtt.dll/.so、libnetleaf_mqtt_server.dll/.so

# ============================================================
# MQTT 客户端模块（可选）
# ============================================================

if(BUILD_MQTT)
    message(STATUS "Building mqtt module...")

    # MQTT 源文件
    set(MQTT_SOURCES
        netleaf_mqtt.c
    )

    # 启用 TLS 时附带 MQTT over TLS 实现
    if(BUILD_TLS)
        list(APPEND MQTT_SOURCES netleaf_mqtt_tls.c)
    endif()

    add_library(netleaf_mqtt SHARED ${MQTT_SOURCES})
    set_target_properties(netleaf_mqtt PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        OUTPUT_NAME "netleaf_mqtt"
    )

    target_include_directories(netleaf_mqtt PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_compile_definitions(netleaf_mqtt PRIVATE
        NL_MQTT_EXPORTS
    )

    if(BUILD_TLS)
        target_compile_definitions(netleaf_mqtt PRIVATE NL_MQTT_TLS_ENABLE)
    endif()

    # 链接核心库、语言扩展，以及可选的 TLS 扩展
    target_link_libraries(netleaf_mqtt PUBLIC netleaf_core netleaf_lang)
    if(BUILD_TLS)
        target_link_libraries(netleaf_mqtt PRIVATE netleaf_tls mbedtls mbedx509 mbedcrypto)
    endif()

    # 平台相关链接
    if(WIN32)
        target_link_libraries(netleaf_mqtt PRIVATE ws2_32)
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(netleaf_mqtt PRIVATE Threads::Threads)
    endif()

    # 安装规则
    install(TARGETS netleaf_mqtt
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
    )
    install(FILES ${CMAKE_SOURCE_DIR}/include/netleaf_mqtt.h
        DESTINATION include/netleaf
    )
endif()

# ============================================================
# MQTT 服务端模块（可选）
# ============================================================

if(BUILD_MQTT_SERVER)
    message(STATUS "Building mqtt_server module...")

    add_library(netleaf_mqtt_server SHARED
        netleaf_mqtt_server.c
    )
    set_target_properties(netleaf_mqtt_server PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        OUTPUT_NAME "netleaf_mqtt_server"
    )

    target_include_directories(netleaf_mqtt_server PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_compile_definitions(netleaf_mqtt_server PRIVATE
        NL_MQTT_SERVER_EXPORTS
    )

    # 链接核心库、语言扩展与 MQTT 客户端
    target_link_libraries(netleaf_mqtt_server PUBLIC netleaf_core netleaf_lang netleaf_mqtt)

    # 启用 TLS 时：服务端 TLS 复用共享 netleaf_tls 扩展(其内部使用 mbedTLS)
    if(BUILD_TLS)
        target_compile_definitions(netleaf_mqtt_server PRIVATE NL_MQTT_SERVER_TLS_ENABLE)
        target_link_libraries(netleaf_mqtt_server PRIVATE netleaf_tls mbedtls mbedx509 mbedcrypto)
    endif()

    # 平台相关链接
    if(WIN32)
        target_link_libraries(netleaf_mqtt_server PRIVATE ws2_32)
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(netleaf_mqtt_server PRIVATE Threads::Threads)
    endif()
endif()
