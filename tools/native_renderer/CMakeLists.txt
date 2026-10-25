cmake_minimum_required(VERSION 3.25)
project(forza_native_renderer_tools LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()
add_executable(native_render_graph_test ../../forza-recomp/tests/native_render_graph_test.cpp)
add_test(NAME native_render_graph COMMAND native_render_graph_test)
add_executable(native_velocity_test ../../forza-recomp/tests/native_velocity_test.cpp)
add_test(NAME native_velocity COMMAND native_velocity_test)
add_executable(native_resolve_test ../../forza-recomp/tests/native_resolve_test.cpp)
add_test(NAME native_resolve COMMAND native_resolve_test)
add_executable(native_geometry_test ../../forza-recomp/tests/native_geometry_test.cpp)
add_test(NAME native_geometry COMMAND native_geometry_test)
add_executable(native_vertex_lit_test ../../forza-recomp/tests/native_vertex_lit_test.cpp)
add_test(NAME native_vertex_lit COMMAND native_vertex_lit_test)
add_executable(native_world_color_test ../../forza-recomp/tests/native_world_color_test.cpp)
add_test(NAME native_world_color COMMAND native_world_color_test)
add_executable(native_scene_transfer_test ../../forza-recomp/tests/native_scene_transfer_test.cpp)
add_test(NAME native_scene_transfer COMMAND native_scene_transfer_test)

option(NATIVE_RENDERER_GPU_TESTS "Build and run synthetic Vulkan pass validation" OFF)
option(NATIVE_RENDERER_VULKAN_VALIDATION "Require Khronos synchronization validation in the GPU test" OFF)
if(NATIVE_RENDERER_VULKAN_VALIDATION AND NOT NATIVE_RENDERER_GPU_TESTS)
    message(FATAL_ERROR "Vulkan validation requires NATIVE_RENDERER_GPU_TESTS")
endif()
if(NATIVE_RENDERER_GPU_TESTS)
    find_package(Vulkan REQUIRED COMPONENTS glslangValidator)
    set(native_source "${CMAKE_CURRENT_SOURCE_DIR}/../../forza-recomp/src/platform/native_renderer")
    # Compile the authored material separately from the SDK's guest shader ABI.
    # These binaries are also suitable inputs for offline Metal translation.
    foreach(stage vert frag)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vertex_lit.${stage}.spv"
            COMMAND Vulkan::glslangValidator -V --target-env vulkan1.1
                "${native_source}/vertex_lit.${stage}"
                -o "${CMAKE_CURRENT_BINARY_DIR}/vertex_lit.${stage}.spv"
            DEPENDS "${native_source}/vertex_lit.${stage}" "${native_source}/vertex_lit.glsl"
            VERBATIM)
    endforeach()
    add_custom_target(native_vertex_lit_shaders ALL DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/vertex_lit.vert.spv"
        "${CMAKE_CURRENT_BINARY_DIR}/vertex_lit.frag.spv")
    add_library(native_velocity_pass STATIC "${native_source}/velocity_pass.cpp")
    target_link_libraries(native_velocity_pass PUBLIC Vulkan::Headers)
    add_library(native_geometry_pass STATIC "${native_source}/geometry_pass.cpp")
    target_link_libraries(native_geometry_pass PUBLIC Vulkan::Headers)
    add_library(native_world_color_pass STATIC "${native_source}/world_color_pass.cpp")
    target_link_libraries(native_world_color_pass PUBLIC Vulkan::Headers)
    foreach(stage vert frag)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/world_color.${stage}.spv"
            COMMAND Vulkan::glslangValidator -V --target-env vulkan1.1
                "${native_source}/world_color.${stage}"
                -o "${CMAKE_CURRENT_BINARY_DIR}/world_color.${stage}.spv"
            DEPENDS "${native_source}/world_color.${stage}" "${native_source}/world_color.glsl"
            VERBATIM)
    endforeach()
    add_custom_target(native_world_color_shaders ALL DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/world_color.vert.spv"
        "${CMAKE_CURRENT_BINARY_DIR}/world_color.frag.spv")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/world_color_view.frag.spv"
        COMMAND Vulkan::glslangValidator -V --target-env vulkan1.1
            "${CMAKE_CURRENT_SOURCE_DIR}/../../forza-recomp/tests/native_world_color_view.frag"
            -o "${CMAKE_CURRENT_BINARY_DIR}/world_color_view.frag.spv"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../forza-recomp/tests/native_world_color_view.frag"
        VERBATIM)
    add_custom_target(native_world_color_view_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/world_color_view.frag.spv")
    add_executable(native_world_color_gpu_test ../../forza-recomp/tests/native_world_color_gpu_test.cpp)
    target_link_libraries(native_world_color_gpu_test PRIVATE native_world_color_pass Vulkan::Vulkan)
    add_dependencies(native_world_color_gpu_test native_world_color_shaders)
    add_dependencies(native_world_color_gpu_test native_world_color_view_shader)
    add_executable(native_velocity_gpu_test ../../forza-recomp/tests/native_velocity_gpu_test.cpp)
    target_link_libraries(native_velocity_gpu_test PRIVATE native_velocity_pass Vulkan::Vulkan)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/velocity.spv"
        COMMAND Vulkan::glslangValidator -V --target-env vulkan1.1
            "${native_source}/velocity.comp" -o "${CMAKE_CURRENT_BINARY_DIR}/velocity.spv"
        DEPENDS "${native_source}/velocity.comp" VERBATIM)
    add_custom_target(native_velocity_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/velocity.spv")
    add_dependencies(native_velocity_gpu_test native_velocity_shader)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/resolve.spv"
        COMMAND Vulkan::glslangValidator -V --target-env vulkan1.1
            "${native_source}/resolve.comp" -o "${CMAKE_CURRENT_BINARY_DIR}/resolve.spv"
        DEPENDS "${native_source}/resolve.comp" VERBATIM)
    add_custom_target(native_resolve_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/resolve.spv")
    add_dependencies(native_velocity_gpu_test native_resolve_shader)
    set(validation_arguments)
    if(NATIVE_RENDERER_VULKAN_VALIDATION)
        list(APPEND validation_arguments --validation)
    endif()
    add_test(NAME native_world_color_gpu COMMAND native_world_color_gpu_test
        "${CMAKE_CURRENT_BINARY_DIR}/world_color.vert.spv"
        "${CMAKE_CURRENT_BINARY_DIR}/world_color.frag.spv"
        "--view-probe=${CMAKE_CURRENT_BINARY_DIR}/world_color_view.frag.spv" ${validation_arguments})
    add_test(NAME native_world_color_dynamic_gpu COMMAND native_world_color_gpu_test
        "${CMAKE_CURRENT_BINARY_DIR}/world_color.vert.spv"
        "${CMAKE_CURRENT_BINARY_DIR}/world_color.frag.spv"
        "--view-probe=${CMAKE_CURRENT_BINARY_DIR}/world_color_view.frag.spv" --dynamic-rendering ${validation_arguments})
    set_tests_properties(native_world_color_gpu native_world_color_dynamic_gpu PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
    add_test(NAME native_velocity_gpu COMMAND native_velocity_gpu_test
        "${CMAKE_CURRENT_BINARY_DIR}/velocity.spv" ${validation_arguments})
    set_tests_properties(native_velocity_gpu PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/resolve_r10.spv"
        COMMAND Vulkan::glslangValidator -V --target-env vulkan1.1 -DFORZA_NATIVE_R10=1
            "${native_source}/resolve.comp" -o "${CMAKE_CURRENT_BINARY_DIR}/resolve_r10.spv"
        DEPENDS "${native_source}/resolve.comp" VERBATIM)
    add_custom_target(native_r10_resolve_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/resolve_r10.spv")
    add_dependencies(native_velocity_gpu_test native_r10_resolve_shader)
    add_test(NAME native_r10_resolve_gpu COMMAND native_velocity_gpu_test
        "${CMAKE_CURRENT_BINARY_DIR}/resolve_r10.spv" --resolve-r10 ${validation_arguments})
    set_tests_properties(native_r10_resolve_gpu PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
    add_test(NAME native_r10_direct_gpu COMMAND native_velocity_gpu_test
        "${CMAKE_CURRENT_BINARY_DIR}/resolve_r10.spv" --resolve-direct ${validation_arguments})
    set_tests_properties(native_r10_direct_gpu PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
    add_test(NAME native_resolve_gpu COMMAND native_velocity_gpu_test
        "${CMAKE_CURRENT_BINARY_DIR}/resolve.spv" --resolve-only ${validation_arguments})
    set_tests_properties(native_resolve_gpu PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/geometry.spv"
        COMMAND Vulkan::glslangValidator -V --target-env vulkan1.1
            "${native_source}/geometry.vert" -o "${CMAKE_CURRENT_BINARY_DIR}/geometry.spv"
        DEPENDS "${native_source}/geometry.vert" VERBATIM)
    add_custom_target(native_geometry_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/geometry.spv")
    add_executable(native_geometry_gpu_test ../../forza-recomp/tests/native_geometry_gpu_test.cpp)
    target_link_libraries(native_geometry_gpu_test PRIVATE native_geometry_pass Vulkan::Vulkan)
    add_dependencies(native_geometry_gpu_test native_geometry_shader)
    add_test(NAME native_geometry_gpu COMMAND native_geometry_gpu_test
        "${CMAKE_CURRENT_BINARY_DIR}/geometry.spv" ${validation_arguments})
    set_tests_properties(native_geometry_gpu PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
    add_test(NAME native_geometry_dynamic_gpu COMMAND native_geometry_gpu_test
        "${CMAKE_CURRENT_BINARY_DIR}/geometry.spv" --dynamic-rendering ${validation_arguments})
    set_tests_properties(native_geometry_dynamic_gpu PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
    add_executable(native_vertex_lit_gpu_test ../../forza-recomp/tests/native_vertex_lit_gpu_test.cpp)
    target_link_libraries(native_vertex_lit_gpu_test PRIVATE Vulkan::Vulkan)
    add_dependencies(native_vertex_lit_gpu_test native_vertex_lit_shaders)
    add_test(NAME native_vertex_lit_gpu COMMAND native_vertex_lit_gpu_test
        "${CMAKE_CURRENT_BINARY_DIR}/vertex_lit.vert.spv"
        "${CMAKE_CURRENT_BINARY_DIR}/vertex_lit.frag.spv" ${validation_arguments})
    set_tests_properties(native_vertex_lit_gpu PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
endif()

set(RENDERDOC_INCLUDE_DIR "" CACHE PATH "RenderDoc's renderdoc/api/replay directory")
set(RENDERDOC_LIBRARY "" CACHE FILEPATH "Official RenderDoc replay library/import library")
if(RENDERDOC_INCLUDE_DIR AND RENDERDOC_LIBRARY)
    add_executable(native_renderer_capture ../native_renderer_capture.cpp)
    add_executable(native_renderer_profile ../native_renderer_profile.cpp)
    target_include_directories(native_renderer_profile PRIVATE "${RENDERDOC_INCLUDE_DIR}")
    target_link_libraries(native_renderer_profile PRIVATE "${RENDERDOC_LIBRARY}")
    target_include_directories(native_renderer_capture PRIVATE "${RENDERDOC_INCLUDE_DIR}")
    target_link_libraries(native_renderer_capture PRIVATE "${RENDERDOC_LIBRARY}")
    if(WIN32)
        target_compile_definitions(native_renderer_capture PRIVATE RENDERDOC_PLATFORM_WIN32)
        target_compile_definitions(native_renderer_profile PRIVATE RENDERDOC_PLATFORM_WIN32)
    else()
        target_compile_definitions(native_renderer_capture PRIVATE RENDERDOC_PLATFORM_LINUX)
        target_compile_definitions(native_renderer_profile PRIVATE RENDERDOC_PLATFORM_LINUX)
    endif()
elseif(RENDERDOC_INCLUDE_DIR OR RENDERDOC_LIBRARY)
    message(FATAL_ERROR "Set both RENDERDOC_INCLUDE_DIR and RENDERDOC_LIBRARY to build the capture tool")
endif()
