cmake_minimum_required(VERSION 3.25)
project(sdk_filesystem_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(rexglue 0.10.0 REQUIRED CONFIG)
add_executable(vfs_host_errors ../../test_vfs_host_errors.cpp)
target_link_libraries(vfs_host_errors PRIVATE rex::runtime)
if(WIN32)
    add_custom_command(TARGET vfs_host_errors POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:rex::runtime>" "$<TARGET_FILE_DIR:vfs_host_errors>")
endif()
enable_testing()
add_test(NAME vfs_host_errors COMMAND vfs_host_errors)
