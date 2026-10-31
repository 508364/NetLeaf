# ============================================================
# NetLeaf 测试程序
# ============================================================
# 从顶层 CMakeLists.txt 拆分而来（v2.4.2）。
# 顶层 CMakeLists.txt 在 BUILD_TESTS=ON 时通过 add_subdirectory(test)
# 引入本文件，使测试注册的维护集中在 test/ 目录。
#
# MQTT 开关（BUILD_MQTT / BUILD_MQTT_SERVER）控制 netleaf_mqtt /
# netleaf_mqtt_server 目标是否被构建。本文件通过 generator expression
# 按开关裁剪 test_modules / test_full / test_all 的链接列表，避免
# 未开启 MQTT 时链接 -lnetleaf_mqtt 失败。
#
# 默认 BUILD_MQTT=ON + BUILD_MQTT_SERVER=ON（顶层已设置），因此
# 默认构建下全部测试可正常链接。

# 公共系统库链接（Windows 需要）
if(WIN32)
    set(NL_TEST_SYS_LIBS ws2_32 winmm mswsock bcrypt)
else()
    set(NL_TEST_SYS_LIBS "")
endif()

# MQTT 库链接项（按开关裁剪）
set(NL_TEST_MQTT_LINKS
    $<$<BOOL:${BUILD_MQTT}>:netleaf_mqtt>
    $<$<BOOL:${BUILD_MQTT_SERVER}>:netleaf_mqtt_server>)

# ----------------------------------------------------------------
# 1. test_modules — 全部模块综合测试
# ----------------------------------------------------------------
add_executable(test_modules test_modules.c)
target_link_libraries(test_modules PRIVATE
    netleaf_core netleaf_lang netleaf_ipc netleaf_linkagg
    netleaf_autocomplete netleaf_autoroute netleaf_errorpage netleaf_vue
    ${NL_TEST_MQTT_LINKS})
if(WIN32)
    target_link_libraries(test_modules PRIVATE ${NL_TEST_SYS_LIBS})
endif()
add_test(NAME ModulesTest COMMAND test_modules)

# ----------------------------------------------------------------
# 2. test_full — 全功能特性测试
# ----------------------------------------------------------------
add_executable(test_full test_full.c)
target_link_libraries(test_full PRIVATE
    netleaf_core netleaf_lang netleaf_ipc netleaf_linkagg
    netleaf_autocomplete netleaf_autoroute netleaf_errorpage netleaf_vue
    ${NL_TEST_MQTT_LINKS})
if(WIN32)
    target_link_libraries(test_full PRIVATE ${NL_TEST_SYS_LIBS})
endif()
add_test(NAME FullTest COMMAND test_full)

# ----------------------------------------------------------------
# 3. test_variable_substitution — 变量替换回归测试（v2.4.0）
# ----------------------------------------------------------------
add_executable(test_variable_substitution test_variable_substitution.c)
target_link_libraries(test_variable_substitution PRIVATE netleaf_core netleaf_lang)
add_test(NAME VariableSubstitutionTest COMMAND test_variable_substitution)

# ----------------------------------------------------------------
# 4. test_all — 全部模块综合测试（C 源码按 HAVE_NETLEAF_MQTT 裁剪）
# ----------------------------------------------------------------
add_executable(test_all test_all.c)
target_link_libraries(test_all PRIVATE
    netleaf_core netleaf_lang netleaf_ipc netleaf_linkagg
    netleaf_autocomplete netleaf_autoroute netleaf_errorpage netleaf_vue
    ${NL_TEST_MQTT_LINKS})
target_compile_definitions(test_all PRIVATE
    $<$<BOOL:${BUILD_MQTT}>:HAVE_NETLEAF_MQTT>
    $<$<BOOL:${BUILD_MQTT_SERVER}>:HAVE_NETLEAF_MQTT_SERVER>)
if(WIN32)
    target_link_libraries(test_all PRIVATE ${NL_TEST_SYS_LIBS})
endif()
add_test(NAME AllModulesTest COMMAND test_all)

# ----------------------------------------------------------------
# 5. test_mqtt_frames — MQTT 帧编码/缓冲区扩容回归测试
#    直接包含 netleaf_mqtt.c 实现，不依赖 netleaf_mqtt 库目标
# ----------------------------------------------------------------
add_executable(test_mqtt_frames test_mqtt_frames.c)
target_link_libraries(test_mqtt_frames PRIVATE netleaf_core netleaf_lang)
if(WIN32)
    # 该测试直接包含 netleaf_mqtt.c，会用到 winsock 等系统库
    target_link_libraries(test_mqtt_frames PRIVATE ${NL_TEST_SYS_LIBS})
endif()
target_include_directories(test_mqtt_frames PRIVATE ${CMAKE_SOURCE_DIR}/src/mqtt)
target_compile_definitions(test_mqtt_frames PRIVATE NL_MQTT_STATIC)
add_test(NAME MqttFramesTest COMMAND test_mqtt_frames)

# ----------------------------------------------------------------
# 6. test_http_frames — HTTP/2 帧 + QUIC varint 编解码回环测试
#    直接包含 src/http/netleaf_http_common.c，纯内存回环不触网
# ----------------------------------------------------------------
add_executable(test_http_frames test_http_frames.c)
target_link_libraries(test_http_frames PRIVATE netleaf_core)
if(WIN32)
    target_link_libraries(test_http_frames PRIVATE ${NL_TEST_SYS_LIBS})
endif()
target_include_directories(test_http_frames PRIVATE
    ${CMAKE_SOURCE_DIR}/src/http
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/optimize)
# 直接包含 netleaf_http_common.c：符号在本可执行文件内定义，
# 定义 NL_HTTP_STATIC 使公共头 NL_API 不标 dllimport，
# 避免 MinGW "redeclared without dllimport attribute" 告警
target_compile_definitions(test_http_frames PRIVATE NL_HTTP_STATIC)
add_test(NAME HttpFramesTest COMMAND test_http_frames)

# ----------------------------------------------------------------
# 7. test_mqtt_server_loopback — MQTT 服务端事件循环/真实下发集成测试
#    回环 socket，需 mqtt + mqtt_server 目标同时开启
# ----------------------------------------------------------------
if(BUILD_MQTT AND BUILD_MQTT_SERVER)
    add_executable(test_mqtt_server_loopback test_mqtt_server_loopback.c)
    target_link_libraries(test_mqtt_server_loopback PRIVATE
        netleaf_core netleaf_lang netleaf_mqtt netleaf_mqtt_server)
    if(WIN32)
        target_link_libraries(test_mqtt_server_loopback PRIVATE ${NL_TEST_SYS_LIBS})
    endif()
    target_include_directories(test_mqtt_server_loopback PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME MqttServerLoopbackTest COMMAND test_mqtt_server_loopback)
endif()

# ----------------------------------------------------------------
# 8. test_quictls — QUIC 扩展（原 quictls + 主库迁出的 H3 纯逻辑）回归测试
#    需 BUILD_QUIC=ON 且 BUILD_TLS3=ON（quic 扩展复用 tls3 的 mbedTLS 目标）。
#    覆盖 KAT / 密钥调度 / 握手+证书 / QUIC+H3 / 可靠性 / 流控 / QPACK。
# ----------------------------------------------------------------
if(BUILD_QUIC AND BUILD_TLS3)
    add_executable(test_quictls test_quictls.c)
    target_link_libraries(test_quictls PRIVATE netleaf_core netleaf_quic)
    if(WIN32)
        target_link_libraries(test_quictls PRIVATE ${NL_TEST_SYS_LIBS})
    endif()
    target_include_directories(test_quictls PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME QuicTlsTest COMMAND test_quictls)

    # 9. h3_server_main — 独立 H3 服务端 harness：供外部 QUIC 客户端（aioquic）连接
    add_executable(h3_server_main quictls_h3_server_main.c)
    target_link_libraries(h3_server_main PRIVATE netleaf_core netleaf_quic)
    if(WIN32)
        target_link_libraries(h3_server_main PRIVATE ${NL_TEST_SYS_LIBS})
    endif()
    target_include_directories(h3_server_main PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

# ----------------------------------------------------------------
# 10. test_web_engine — P12 Web 引擎集成测试
#     覆盖 CONTENT/FILE/REDIRECT/PROXY/HEAD/404 + slowloris 空闲回收
#     + 客户端提前断开健壮性；回环 socket，不使用 mock
# ----------------------------------------------------------------
add_executable(test_web_engine test_web_engine.c)
target_link_libraries(test_web_engine PRIVATE netleaf_core netleaf_lang)
if(WIN32)
    target_link_libraries(test_web_engine PRIVATE ${NL_TEST_SYS_LIBS})
endif()
target_include_directories(test_web_engine PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME WebEngineTest COMMAND test_web_engine)

# ----------------------------------------------------------------
# 11. bench_web — P8 引擎压测工具（Linux/macOS 专用）
#     对比 legacy pool / engine 单实例 / engine 多实例
# ----------------------------------------------------------------
if(NOT WIN32)
    add_executable(bench_web bench_web.c)
    target_link_libraries(bench_web PRIVATE netleaf_core netleaf_lang)
    find_package(Threads REQUIRED)
    target_link_libraries(bench_web PRIVATE Threads::Threads)
    target_include_directories(bench_web PRIVATE ${CMAKE_SOURCE_DIR}/include)
    # 压测工具不注册为 ctest（手动运行）
endif()
