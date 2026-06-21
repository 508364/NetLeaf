# Link Aggregation module CMakeLists
# Part of NetLeaf v${PROJECT_VERSION}
# This module was added in v2.2.1 for same-port link aggregation

set(LINKAGG_SOURCES
    netleaf_linkagg.c
)

# Linux-specific source
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND LINKAGG_SOURCES
        ${CMAKE_SOURCE_DIR}/src/linux/netleaf_linkagg_linux.c
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND LINKAGG_SOURCES
        ${CMAKE_SOURCE_DIR}/src/windows/netleaf_linkagg_windows.c
    )
else()
    # macOS: skip link aggregation module
    message(STATUS "  - Link aggregation skipped (macOS not supported)")
    return()
endif()

# Create object library (compile once, link to both static and shared)
add_library(netleaf_linkagg_obj OBJECT ${LINKAGG_SOURCES})

target_include_directories(netleaf_linkagg_obj PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/lang
)

target_compile_definitions(netleaf_linkagg_obj PUBLIC
    NL_VERSION="${PROJECT_VERSION}"
    NL_PLATFORM_${CMAKE_SYSTEM_NAME}
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(netleaf_linkagg_obj PUBLIC
        NL_LINKAGG_EXPORTS
    )
endif()

# Create static library from object library
add_library(netleaf_linkagg STATIC $<TARGET_OBJECTS:netleaf_linkagg_obj>)

target_compile_definitions(netleaf_linkagg PUBLIC
    NL_LINKAGG_STATIC
)

target_link_libraries(netleaf_linkagg PUBLIC
    ${LINK_LIBS}
    netleaf_ipc
    netleaf_lang
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(netleaf_linkagg PUBLIC ws2_32 legacy_stdio_definitions)
endif()

set_target_properties(netleaf_linkagg PROPERTIES
    OUTPUT_NAME "netleaf_linkagg"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Create shared library from object library
add_library(netleaf_linkagg_shared SHARED $<TARGET_OBJECTS:netleaf_linkagg_obj>)

# NL_LINKAGG_EXPORTS is now defined in netleaf_linkagg_obj for shared builds

target_link_libraries(netleaf_linkagg_shared PUBLIC ${LINK_LIBS} netleaf_ipc_shared netleaf_lang_shared)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(netleaf_linkagg_shared PRIVATE ws2_32 legacy_stdio_definitions)
endif()

set_target_properties(netleaf_linkagg_shared PROPERTIES
    OUTPUT_NAME "netleaf_linkagg"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

message(STATUS "  - Link Aggregation v${PROJECT_VERSION} (same-port load balancing)")
