# Auto-complete module CMakeLists
# Part of NetLeaf v${PROJECT_VERSION}
# This module was separated from the main NetLeaf library

set(AUTOCOMPLETE_SOURCES
    netleaf_autocomplete.c
    netleaf_charset.c
    netleaf_vue_import.c
)

add_library(netleaf_autocomplete STATIC ${AUTOCOMPLETE_SOURCES})

target_include_directories(netleaf_autocomplete PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(netleaf_autocomplete PUBLIC
    NL_VERSION="${PROJECT_VERSION}"
    NL_PLATFORM_${PLATFORM_NAME}
    NL_AUTOCOMPLETE_STATIC
)

target_link_libraries(netleaf_autocomplete PUBLIC
    ${LINK_LIBS}
)

set_target_properties(netleaf_autocomplete PROPERTIES
    OUTPUT_NAME "netleaf_autocomplete"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Create shared version as well
add_library(netleaf_autocomplete_shared SHARED ${AUTOCOMPLETE_SOURCES})
target_compile_definitions(netleaf_autocomplete_shared PRIVATE 
    NL_AUTOCOMPLETE_EXPORTS
    NL_VERSION="${PROJECT_VERSION}"
    NL_PLATFORM_${PLATFORM_NAME}
)
target_include_directories(netleaf_autocomplete_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(netleaf_autocomplete_shared PUBLIC ${LINK_LIBS})
set_target_properties(netleaf_autocomplete_shared PROPERTIES
    OUTPUT_NAME "netleaf_autocomplete"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

message(STATUS "  - Autocomplete v${PROJECT_VERSION} (charset/Vue auto-complete)")