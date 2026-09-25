# IPC 扩展模块构建脚本
# 进程间通信（v2.2.1 引入）
# 目标产物：libnetleaf_ipc.dll/.so

message(STATUS "Building ipc module...")

# 平台相关源文件
if(WIN32)
    set(IPC_PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/windows/netleaf_ipc_windows.c)
elseif(APPLE)
    set(IPC_PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/macos/netleaf_ipc_macos.c)
elseif(UNIX)
    set(IPC_PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/linux/netleaf_ipc_linux.c)
endif()

add_library(netleaf_ipc SHARED
    netleaf_ipc.c
    ${IPC_PLATFORM_SOURCES}
)
set_target_properties(netleaf_ipc PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "netleaf_ipc"
)
target_include_directories(netleaf_ipc PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(netleaf_ipc PRIVATE NL_IPC_EXPORTS)
target_link_libraries(netleaf_ipc PUBLIC netleaf_core netleaf_lang)

# Windows 平台需要链接 Winsock
if(WIN32)
    target_link_libraries(netleaf_ipc PRIVATE ws2_32)
endif()

# 安装规则
install(TARGETS netleaf_ipc
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES ${CMAKE_SOURCE_DIR}/include/netleaf_ipc.h
    DESTINATION include
)
