cmake_minimum_required(VERSION 3.25)
project(vulkan_constant_bindings_test LANGUAGES CXX)
find_package(Vulkan REQUIRED COMPONENTS glslangValidator)
add_executable(vulkan_constant_bindings_test constants_test.cpp)
target_compile_features(vulkan_constant_bindings_test PRIVATE cxx_std_17)
target_link_libraries(vulkan_constant_bindings_test PRIVATE Vulkan::Vulkan)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/constants.spv"
  COMMAND Vulkan::glslangValidator -V "${CMAKE_CURRENT_SOURCE_DIR}/constants.comp"
          -o "${CMAKE_CURRENT_BINARY_DIR}/constants.spv"
  DEPENDS constants.comp VERBATIM)
add_custom_target(constants_shader ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/constants.spv")
add_dependencies(vulkan_constant_bindings_test constants_shader)
enable_testing()
add_test(NAME vulkan_constant_bindings
  COMMAND vulkan_constant_bindings_test "${CMAKE_CURRENT_BINARY_DIR}/constants.spv" --validation)
