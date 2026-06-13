# ErrorPage module CMakeLists
# Part of NetLeaf v${PROJECT_VERSION}
# This module was separated from the main NetLeaf library

add_library(netleaf_errorpage STATIC
    netleaf_errorpage.c
)

target_include_directories(netleaf_errorpage PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(netleaf_errorpage PUBLIC
    NL_VERSION="${PROJECT_VERSION}"
    NL_PLATFORM_${PLATFORM_NAME}
    NL_ERRORPAGE_STATIC
)

target_link_libraries(netleaf_errorpage PUBLIC
    ${LINK_LIBS}
)

set_target_properties(netleaf_errorpage PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Create shared version as well
add_library(netleaf_errorpage_shared SHARED netleaf_errorpage.c)
target_compile_definitions(netleaf_errorpage_shared PRIVATE 
    NL_ERRORPAGE_EXPORTS
    NL_VERSION="${PROJECT_VERSION}"
    NL_PLATFORM_${PLATFORM_NAME}
)
target_include_directories(netleaf_errorpage_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(netleaf_errorpage_shared PUBLIC ${LINK_LIBS})
set_target_properties(netleaf_errorpage_shared PROPERTIES
    OUTPUT_NAME "netleaf_errorpage"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

message(STATUS "  - ErrorPage v${PROJECT_VERSION} (template-based error pages)")