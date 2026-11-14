cmake_minimum_required(VERSION 3.16)
project(ExamplePlugin VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# NetLeaf root
set(NETLEAF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

include_directories(
    ${NETLEAF_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Plugin sources
set(PLUGIN_SOURCES
    example_plugin.c
)

# Build as shared library
add_library(example_plugin SHARED ${PLUGIN_SOURCES})

set_target_properties(example_plugin PROPERTIES
    OUTPUT_NAME "example_plugin"
    PREFIX ""
)

if(WIN32)
    set_target_properties(example_plugin PROPERTIES SUFFIX ".dll")
endif()

# Export symbols
target_compile_definitions(example_plugin PRIVATE
    NL_PLUGIN_EXPORTS
)

# Link against NetLeaf if needed
# target_link_libraries(example_plugin PRIVATE netleaf)

message(STATUS "Building ExamplePlugin v${PROJECT_VERSION}")
