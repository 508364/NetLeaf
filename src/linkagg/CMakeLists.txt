# LinkAgg 扩展模块构建脚本
# 同端口链路聚合与负载均衡（v2.2.1 引入）
# 目标产物：libnetleaf_linkagg.dll/.so

message(STATUS "Building linkagg module...")

# 平台相关源文件
if(WIN32)
    set(LINKAGG_PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/windows/netleaf_linkagg_windows.c)
elseif(APPLE)
    set(LINKAGG_PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/macos/netleaf_linkagg_macos.c)
elseif(UNIX)
    set(LINKAGG_PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/linux/netleaf_linkagg_linux.c)
endif()

add_library(netleaf_linkagg SHARED
    netleaf_linkagg.c
    ${LINKAGG_PLATFORM_SOURCES}
)
set_target_properties(netleaf_linkagg PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "netleaf_linkagg"
)
target_include_directories(netleaf_linkagg PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(netleaf_linkagg PRIVATE NL_LINKAGG_EXPORTS)
target_link_libraries(netleaf_linkagg PUBLIC netleaf_core netleaf_lang netleaf_ipc)

# POSIX 下使用 pthread(互斥量)，必须链接线程库
if(UNIX)
    target_link_libraries(netleaf_linkagg PUBLIC Threads::Threads)
endif()

# Windows 平台需要链接 Winsock
if(WIN32)
    target_link_libraries(netleaf_linkagg PRIVATE ws2_32)
endif()

# 安装规则
install(TARGETS netleaf_linkagg
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES ${CMAKE_SOURCE_DIR}/include/netleaf_linkagg.h
    DESTINATION include
)
