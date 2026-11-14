cmake_minimum_required(VERSION 3.16)
project(MyNetLeafPlugin VERSION 1.0.0 LANGUAGES C)

# =========================================
# Plugin Build Configuration
# =========================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Plugin name (use your plugin's name)
set(PLUGIN_NAME "my_plugin")

# =========================================
# NetLeaf Integration
# =========================================

# Find NetLeaf headers and libraries
set(NETLEAF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "NetLeaf root directory")

# Include directories
include_directories(
    ${NETLEAF_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# =========================================
# Plugin Source Files
# =========================================

set(PLUGIN_SOURCES
    plugin.c
    netleaf_plugin_template.h
)

# =========================================
# Build as Shared Library (Plugin)
# =========================================

add_library(${PLUGIN_NAME} SHARED ${PLUGIN_SOURCES})

# Set plugin output name
set_target_properties(${PLUGIN_NAME} PROPERTIES
    OUTPUT_NAME "${PLUGIN_NAME}"
    PREFIX ""  # Don't add 'lib' prefix on Unix
)

# Windows: Add .dll extension
if(WIN32)
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        SUFFIX ".dll"
    )
endif()

# =========================================
# Compiler Definitions
# =========================================

# Export symbols for plugin
target_compile_definitions(${PLUGIN_NAME} PRIVATE
    NL_PLUGIN_EXPORTS
    PLUGIN_NAME=\"${PLUGIN_NAME}\"
)

# Platform-specific definitions
if(WIN32)
    target_compile_definitions(${PLUGIN_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
elseif(APPLE)
    target_compile_definitions(${PLUGIN_NAME} PRIVATE
        __APPLE__
    )
elseif(UNIX)
    target_compile_definitions(${PLUGIN_NAME} PRIVATE
        __linux__
    )
endif()

# =========================================
# Link Libraries (if needed)
# =========================================

# Link against NetLeaf if your plugin uses its API
# Uncomment if needed:
# target_link_libraries(${PLUGIN_NAME} PRIVATE netleaf)

# =========================================
# Install Targets
# =========================================

install(TARGETS ${PLUGIN_NAME}
    LIBRARY DESTINATION extensions
    RUNTIME DESTINATION extensions
)

# =========================================
# Build Configuration (Optional)
# =========================================

# Release build (default)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compiler optimizations for release
target_compile_options(${PLUGIN_NAME} PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-O0 -g>
)

message(STATUS "Building plugin: ${PLUGIN_NAME} v${PROJECT_VERSION}")
message(STATUS "Output: ${CMAKE_BINARY_DIR}/${PLUGIN_NAME}.${CMAKE_SHARED_LIBRARY_SUFFIX}")
