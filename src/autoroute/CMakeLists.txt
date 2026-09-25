# AutoRoute 扩展模块构建脚本
# 智能路由与路由匹配
# 目标产物：libnetleaf_autoroute.dll/.so

message(STATUS "Building autoroute module...")

add_library(netleaf_autoroute SHARED
    netleaf_autoroute.c
    netleaf_route_matcher.c
)
set_target_properties(netleaf_autoroute PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "netleaf_autoroute"
)
target_include_directories(netleaf_autoroute PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(netleaf_autoroute PRIVATE NL_AUTOROUTE_EXPORTS)
target_link_libraries(netleaf_autoroute PUBLIC netleaf_core netleaf_lang)

# 安装规则
install(TARGETS netleaf_autoroute
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES ${CMAKE_SOURCE_DIR}/include/netleaf_autoroute.h
    DESTINATION include
)
