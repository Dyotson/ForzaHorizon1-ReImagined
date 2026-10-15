# Build on Windows with clang-cl and lld-link (from a portable LLVM) against the MSVC CRT and
# Windows SDK: either a Visual Studio / Build Tools install (run from its developer prompt, or
# let the launcher import vcvars64.bat) or the files `xwin splat` unpacks. The compilers and
# the INCLUDE and LIB variables come from the environment; the launcher sets them.
# The cross build from Linux has its own file, windows-clang-cl.cmake.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

find_program(_clang_cl clang-cl HINTS ENV FORZA_LLVM_BIN REQUIRED)
find_program(_lld_link lld-link HINTS ENV FORZA_LLVM_BIN REQUIRED)
find_program(_llvm_lib llvm-lib HINTS ENV FORZA_LLVM_BIN REQUIRED)
find_program(_llvm_rc llvm-rc HINTS ENV FORZA_LLVM_BIN)
set(CMAKE_C_COMPILER "${_clang_cl}")
set(CMAKE_CXX_COMPILER "${_clang_cl}")
set(CMAKE_LINKER "${_lld_link}")
set(CMAKE_AR "${_llvm_lib}")
if(_llvm_rc)
    set(CMAKE_RC_COMPILER "${_llvm_rc}")
endif()
set(CMAKE_MT "")  # lld-link embeds the manifest itself

# SSE4.1 as on Linux: the generated code rounds with roundevenf, which the MSVC CRT lacks.
set(CMAKE_C_FLAGS_INIT "--target=x86_64-pc-windows-msvc -fuse-ld=lld-link /clang:-msse4.1")
# No calls into the STL's vectorized helpers (__std_find_first_of_trivial_pos_1 and friends):
# they only exist in recent MSVC runtimes, so an SDK built with the newest MSVC failed to link
# on a player's older Visual Studio 2022 (#64). The plain templates work with any of them.
set(CMAKE_CXX_FLAGS_INIT "--target=x86_64-pc-windows-msvc -fuse-ld=lld-link /clang:-msse4.1 /D_USE_STD_VECTOR_ALGORITHMS=0")
set(CMAKE_EXE_LINKER_FLAGS_INIT "/manifest:embed")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "/manifest:embed")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "/manifest:embed")

# Only the release CRT is guaranteed (xwin unpacks no debug libraries).
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
