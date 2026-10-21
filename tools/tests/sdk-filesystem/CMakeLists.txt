cmake_minimum_required(VERSION 3.25)
project(sdk_filesystem_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(rexglue 0.10.0 REQUIRED CONFIG)
add_executable(photo_directories photo_directories.cpp)
target_link_libraries(photo_directories PRIVATE rex::runtime)
if(WIN32)
    add_custom_command(TARGET photo_directories POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:rex::runtime>" "$<TARGET_FILE_DIR:photo_directories>")
endif()
enable_testing()
add_test(NAME photo_directories COMMAND photo_directories "${CMAKE_CURRENT_BINARY_DIR}/scratch")
