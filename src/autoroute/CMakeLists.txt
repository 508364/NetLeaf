# Auto-route module CMakeLists
# Part of NetLeaf v${PROJECT_VERSION}
# This module was separated from the main NetLeaf library

set(AUTOROUTE_SOURCES
    netleaf_autoroute.c
    netleaf_route_matcher.c
)

add_library(netleaf_autoroute STATIC ${AUTOROUTE_SOURCES})

target_include_directories(netleaf_autoroute PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(netleaf_autoroute PUBLIC
    NL_VERSION="${PROJECT_VERSION}"
    NL_PLATFORM_${PLATFORM_NAME}
    NL_AUTOROUTE_STATIC
)

target_link_libraries(netleaf_autoroute PUBLIC
    ${LINK_LIBS}
)

set_target_properties(netleaf_autoroute PROPERTIES
    OUTPUT_NAME "netleaf_autoroute"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Create shared version as well
add_library(netleaf_autoroute_shared SHARED ${AUTOROUTE_SOURCES})
target_compile_definitions(netleaf_autoroute_shared PRIVATE 
    NL_AUTOROUTE_EXPORTS
    NL_VERSION="${PROJECT_VERSION}"
    NL_PLATFORM_${PLATFORM_NAME}
)
target_include_directories(netleaf_autoroute_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(netleaf_autoroute_shared PUBLIC ${LINK_LIBS})
set_target_properties(netleaf_autoroute_shared PROPERTIES
    OUTPUT_NAME "netleaf_autoroute"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

message(STATUS "  - Auto-route v${PROJECT_VERSION} (route suggestions)")