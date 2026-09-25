# AutoComplete 扩展模块构建脚本
# 字符集补全 + Vue 自动导入
# 目标产物：libnetleaf_autocomplete.dll/.so

message(STATUS "Building autocomplete module...")

add_library(netleaf_autocomplete SHARED
    netleaf_autocomplete.c
    netleaf_charset.c
    netleaf_autocomplete_vue.c
)
set_target_properties(netleaf_autocomplete PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "netleaf_autocomplete"
)
target_include_directories(netleaf_autocomplete PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(netleaf_autocomplete PRIVATE NL_AUTOCOMPLETE_EXPORTS)
target_link_libraries(netleaf_autocomplete PUBLIC netleaf_core netleaf_lang netleaf_vue)

# 安装规则
install(TARGETS netleaf_autocomplete
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES ${CMAKE_SOURCE_DIR}/include/netleaf_autocomplete.h
    DESTINATION include
)
