set(VUE_SOURCES
    netleaf_vue.c
)

add_library(netleaf_vue_obj OBJECT ${VUE_SOURCES})

target_include_directories(netleaf_vue_obj PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(netleaf_vue_obj PUBLIC
    NL_VERSION="${PROJECT_VERSION}"
    NL_PLATFORM_${CMAKE_SYSTEM_NAME}
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(netleaf_vue_obj PUBLIC
        NL_VUE_EXPORTS
    )
endif()

add_library(netleaf_vue STATIC $<TARGET_OBJECTS:netleaf_vue_obj>)

target_compile_definitions(netleaf_vue PUBLIC
    NL_VUE_STATIC
)

target_link_libraries(netleaf_vue PUBLIC
    ${LINK_LIBS}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(netleaf_vue PUBLIC ws2_32 legacy_stdio_definitions)
endif()

set_target_properties(netleaf_vue PROPERTIES
    OUTPUT_NAME "netleaf_vue"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

add_library(netleaf_vue_shared SHARED $<TARGET_OBJECTS:netleaf_vue_obj>)

target_link_libraries(netleaf_vue_shared PUBLIC ${LINK_LIBS})

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(netleaf_vue_shared PRIVATE ws2_32 legacy_stdio_definitions)
endif()

set_target_properties(netleaf_vue_shared PROPERTIES
    OUTPUT_NAME "netleaf_vue"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

message(STATUS "  - Vue v${PROJECT_VERSION} (Vue.js backend support)")