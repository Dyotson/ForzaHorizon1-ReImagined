cmake_minimum_required(VERSION 3.25)
project(guest_write_transaction_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(rexglue 0.10.0 REQUIRED CONFIG)
add_executable(guest_write_transaction_test ../guest_write_transaction_test.cpp)
target_link_libraries(guest_write_transaction_test PRIVATE rex::runtime)
if(MSVC)
    target_compile_options(guest_write_transaction_test PRIVATE /UNDEBUG)
else()
    target_compile_options(guest_write_transaction_test PRIVATE -UNDEBUG)
endif()
if(WIN32)
    add_custom_command(TARGET guest_write_transaction_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:rex::runtime>" "$<TARGET_FILE_DIR:guest_write_transaction_test>"
        VERBATIM)
endif()

enable_testing()
add_test(NAME guest_write_transaction COMMAND guest_write_transaction_test)
set_tests_properties(guest_write_transaction PROPERTIES TIMEOUT 10)
