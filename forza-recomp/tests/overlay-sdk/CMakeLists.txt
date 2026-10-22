cmake_minimum_required(VERSION 3.25)
project(overlay_sdk_compatibility LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use the unmodified pinned nightly, just as the Linux launcher does. No game
# files or generated guest code are needed to compile the actual HUD adapter.
find_package(rexglue 0.10.0 REQUIRED CONFIG)
add_library(stock_sdk_overlay OBJECT
    ../../src/platform/hud_overlay.cpp
    ../../src/platform/sdk_import.cpp)
target_link_libraries(stock_sdk_overlay PRIVATE rex::runtime)

if(NOT EXISTS "${FORZA_PATCHED_SDK_SOURCE}/src/ui/imgui_dialog_repaint.h")
    message(FATAL_ERROR "FORZA_PATCHED_SDK_SOURCE must contain applied patch 0078")
endif()
add_executable(dialog_repaint_test dialog_repaint_test.cpp)
target_include_directories(dialog_repaint_test PRIVATE "${FORZA_PATCHED_SDK_SOURCE}/src/ui")

if(NOT EXISTS "${FORZA_PATCHED_SDK_SOURCE}/src/audio/sdl/audio_output_fallback.h")
    message(FATAL_ERROR "FORZA_PATCHED_SDK_SOURCE must contain applied patch 0103")
endif()
add_executable(audio_output_fallback_test audio_output_fallback_test.cpp)
target_include_directories(audio_output_fallback_test PRIVATE
    "${FORZA_PATCHED_SDK_SOURCE}/src/audio/sdl")

enable_testing()
add_test(NAME dialog_repaint COMMAND dialog_repaint_test)
add_test(NAME audio_output_fallback COMMAND audio_output_fallback_test)
