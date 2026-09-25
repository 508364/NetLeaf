# Vue 扩展模块构建脚本
# Vue.js 模板引擎后端支持
# 目标产物：libnetleaf_vue.dll/.so

message(STATUS "Building vue module...")

add_library(netleaf_vue SHARED
    netleaf_vue.c
)
set_target_properties(netleaf_vue PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "netleaf_vue"
)
target_include_directories(netleaf_vue PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(netleaf_vue PRIVATE NL_VUE_EXPORTS)
target_link_libraries(netleaf_vue PUBLIC netleaf_core netleaf_lang)

# 安装规则
install(TARGETS netleaf_vue
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES ${CMAKE_SOURCE_DIR}/include/netleaf_vue.h
    DESTINATION include
)
