cmake_minimum_required(VERSION 3.10)
project(netleaf_example_plugin)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)

add_library(netleaf_example_plugin SHARED netleaf_example_plugin.c)

target_compile_definitions(netleaf_example_plugin PRIVATE NL_EXPORTS)

if(WIN32)
    target_link_libraries(netleaf_example_plugin netleaf)
else()
    target_link_libraries(netleaf_example_plugin -L${CMAKE_CURRENT_SOURCE_DIR}/../../build -lnetleaf)
endif()

install(TARGETS netleaf_example_plugin DESTINATION plugins)
