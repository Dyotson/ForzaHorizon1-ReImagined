# Discord's MIT-licensed classic Rich Presence client, built into the game. Sources
# are pinned and hash-checked; no Discord binaries, tokens or game data are shipped.
include(FetchContent)
FetchContent_Declare(forza_discord_rpc
    URL https://codeload.github.com/discord/discord-rpc/tar.gz/963aa9f3e5ce81a4682c6ca3d136cddda614db33
    URL_HASH SHA256=8f4115999391f95f443615fc5b13cb2398926ac4c418d3d34ff5735f71c6faea
    SOURCE_SUBDIR unused)
FetchContent_Declare(forza_rapidjson
    URL https://codeload.github.com/Tencent/rapidjson/tar.gz/24b5e7a8b27f42fa16b96fc70aade9106cf7102f
    URL_HASH SHA256=2d2601a82d2d3b7e143a3c8d43ef616671391034bc46891a9816b79cf2d3e7a8
    SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(forza_discord_rpc forza_rapidjson)
set(_rpc "${forza_discord_rpc_SOURCE_DIR}")
add_library(forza-discord-rpc STATIC
    ${_rpc}/src/discord_rpc.cpp ${_rpc}/src/rpc_connection.cpp ${_rpc}/src/serialization.cpp)
target_include_directories(forza-discord-rpc PUBLIC ${_rpc}/include
    PRIVATE ${forza_rapidjson_SOURCE_DIR}/include ${_rpc}/src)
if(WIN32)
    set(_connection ${_rpc}/src/connection_win.cpp)
    target_sources(forza-discord-rpc PRIVATE ${_rpc}/src/discord_register_win.cpp)
    target_compile_definitions(forza-discord-rpc PRIVATE DISCORD_WINDOWS)
    target_link_libraries(forza-discord-rpc PRIVATE psapi advapi32)
else()
    set(_connection ${CMAKE_CURRENT_LIST_DIR}/../src/platform/discord_transport_unix.cpp)
    target_sources(forza-discord-rpc PRIVATE ${_rpc}/src/discord_register_linux.cpp)
    target_compile_definitions(forza-discord-rpc PRIVATE DISCORD_LINUX)
    target_link_libraries(forza-discord-rpc PRIVATE pthread)
endif()
if(FORZA_DISCORD_TEST_TRANSPORT AND WIN32)
    # Tests must never find or modify the user's real Discord activity. Change only
    # the endpoint prefix in a build-local copy of the pinned transport source.
    file(READ "${_connection}" _connection_text)
    string(REPLACE "discord-ipc-" "forza-discord-test-ipc-" _connection_text "${_connection_text}")
    set(_connection "${CMAKE_CURRENT_BINARY_DIR}/discord_test_connection.cpp")
    file(CONFIGURE OUTPUT "${_connection}" CONTENT "${_connection_text}" @ONLY)
endif()
if(FORZA_DISCORD_TEST_TRANSPORT)
    target_compile_definitions(forza-discord-rpc PRIVATE FORZA_DISCORD_TEST_TRANSPORT)
endif()
target_sources(forza-discord-rpc PRIVATE "${_connection}")
target_link_libraries(forza PRIVATE forza-discord-rpc)
add_custom_command(TARGET forza POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:forza>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_rpc}/LICENSE"
        "$<TARGET_FILE_DIR:forza>/licenses/discord-rpc.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${forza_rapidjson_SOURCE_DIR}/license.txt"
        "$<TARGET_FILE_DIR:forza>/licenses/rapidjson.txt"
    VERBATIM)
