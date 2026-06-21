# Lang Module CMakeLists.txt
# Multi-language error message support

# Module info
message(STATUS "Building lang module...")
message(STATUS "  - Lang v${PROJECT_VERSION} (multi-language error messages)")

# Create static library
add_library(netleaf_lang STATIC
    netleaf_lang.c
)

target_include_directories(netleaf_lang PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(netleaf_lang PRIVATE
    NL_LANG_EXPORTS
)

# Create shared library (optional)
if(BUILD_SHARED_LIBS)
    add_library(netleaf_lang_shared SHARED
        netleaf_lang.c
    )
    
    target_include_directories(netleaf_lang_shared PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    )
    
    set_target_properties(netleaf_lang_shared PROPERTIES
        OUTPUT_NAME "netleaf_lang"
        VERSION ${PROJECT_VERSION}
        SOVERSION 2
    )
endif()

# Installation
install(TARGETS netleaf_lang
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(FILES ${CMAKE_SOURCE_DIR}/include/netleaf_lang.h
    DESTINATION include
)
