# NetLeaf Dynamic Extension Library Example
# This demonstrates how third-party developers can create extension libraries
# that follow NetLeaf's module pattern and support dynamic loading

cmake_minimum_required(VERSION 3.10)
project(netleaf_extension_example)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

# Create the extension library
add_library(netleaf_extension SHARED netleaf_extension_example.c)

# Important: Link against the SHARED main library (netleaf.dll)
# This ensures the extension and main program share the same global state
if(WIN32)
    target_compile_definitions(netleaf_extension PRIVATE NL_EXTENSION_EXPORTS)
    target_link_libraries(netleaf_extension netleaf_shared ws2_32)
else()
    target_link_libraries(netleaf_extension netleaf_shared m dl pthread)
endif()

# Set output name and remove 'lib' prefix on Windows
set_target_properties(netleaf_extension PROPERTIES
    OUTPUT_NAME "netleaf_extension"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Test program to verify extension loading
add_executable(test_extension test_extension.c)

if(WIN32)
    target_link_libraries(test_extension netleaf_shared ws2_32)
else()
    target_link_libraries(test_extension netleaf_shared m dl pthread)
endif()

set_target_properties(test_extension PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

message(STATUS "  - Extension Example v1.0.0 (dynamic loading demo)")