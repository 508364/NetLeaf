# IPC module CMakeLists
# Part of NetLeaf v${PROJECT_VERSION}
# This module was added in v2.2.1 for inter-process communication

set(IPC_SOURCES
    netleaf_ipc.c
)

# Linux-specific source
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND IPC_SOURCES
        ${CMAKE_SOURCE_DIR}/src/linux/netleaf_ipc_linux.c
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND IPC_SOURCES
        ${CMAKE_SOURCE_DIR}/src/windows/netleaf_ipc_windows.c
    )
else()
    # macOS: skip IPC module
    message(STATUS "  - IPC skipped (macOS not supported)")
    return()
endif()

# Create object library (compile once, link to both static and shared)
add_library(netleaf_ipc_obj OBJECT ${IPC_SOURCES})

target_include_directories(netleaf_ipc_obj PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(netleaf_ipc_obj PUBLIC
    NL_VERSION="${PROJECT_VERSION}"
    NL_PLATFORM_${CMAKE_SYSTEM_NAME}
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(netleaf_ipc_obj PUBLIC
        NL_IPC_EXPORTS
    )
endif()

# Create static library from object library
add_library(netleaf_ipc STATIC $<TARGET_OBJECTS:netleaf_ipc_obj>)

target_compile_definitions(netleaf_ipc PUBLIC
    NL_IPC_STATIC
)

target_link_libraries(netleaf_ipc PUBLIC
    ${LINK_LIBS}
)

set_target_properties(netleaf_ipc PROPERTIES
    OUTPUT_NAME "netleaf_ipc"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Create shared library from object library
add_library(netleaf_ipc_shared SHARED $<TARGET_OBJECTS:netleaf_ipc_obj>)

# NL_IPC_EXPORTS is now defined in netleaf_ipc_obj for shared builds

target_link_libraries(netleaf_ipc_shared PUBLIC ${LINK_LIBS})

set_target_properties(netleaf_ipc_shared PROPERTIES
    OUTPUT_NAME "netleaf_ipc"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

message(STATUS "  - IPC v${PROJECT_VERSION} (inter-process communication)")
