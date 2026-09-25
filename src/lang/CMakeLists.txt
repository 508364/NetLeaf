# Lang 扩展模块构建脚本
# 多语言错误消息支持（语言支持模块）
# 目标产物：libnetleaf_lang.dll/.so

message(STATUS "Building lang module...")

add_library(netleaf_lang SHARED
    netleaf_lang.c
)
set_target_properties(netleaf_lang PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "netleaf_lang"
)
target_include_directories(netleaf_lang PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(netleaf_lang PRIVATE NL_LANG_EXPORTS)
target_link_libraries(netleaf_lang PUBLIC netleaf_core)

# POSIX 下使用 pthread(互斥量 + pthread_once)，必须链接线程库
if(UNIX)
    target_link_libraries(netleaf_lang PUBLIC Threads::Threads)
endif()

# Windows 平台需要链接 Winsock
if(WIN32)
    target_link_libraries(netleaf_lang PRIVATE ws2_32)
endif()

# 安装规则
install(TARGETS netleaf_lang
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES ${CMAKE_SOURCE_DIR}/include/netleaf_lang.h
    DESTINATION include
)
