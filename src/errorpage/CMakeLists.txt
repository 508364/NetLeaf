# ErrorPage 扩展模块构建脚本
# 基于模板的错误页面渲染
# 目标产物：libnetleaf_errorpage.dll/.so

message(STATUS "Building errorpage module...")

add_library(netleaf_errorpage SHARED
    netleaf_errorpage.c
)
set_target_properties(netleaf_errorpage PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "netleaf_errorpage"
)
target_include_directories(netleaf_errorpage PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(netleaf_errorpage PRIVATE NL_ERRORPAGE_EXPORTS)
target_link_libraries(netleaf_errorpage PUBLIC netleaf_core netleaf_lang)

# 安装规则
install(TARGETS netleaf_errorpage
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES ${CMAKE_SOURCE_DIR}/include/netleaf_errorpage.h
    DESTINATION include
)
