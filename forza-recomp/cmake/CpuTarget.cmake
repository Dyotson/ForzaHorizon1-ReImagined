# FORZA_CPU_TARGET: the CPU the generated (recompiled) code is compiled for (#220, docs/perf.md).
#   baseline   SSE4.1, the SDK's setting: any x86-64 PC. The default.
#   x86-64-v3  AVX2, FMA, BMI1/2, F16C, LZCNT and MOVBE: Intel since Haswell (2013), AMD since
#              Excavator / Zen, the Steam Deck. The guest's fused multiply-adds become one
#              instruction instead of a math-library call, big-endian loads and stores use MOVBE.
# -ffp-contract=off keeps every float result the same as baseline's: the compiler fuses no
# multiply and add of its own; only the explicit ones (ppc_fma, already fused) use FMA.
# The launcher passes x86-64-v3 when the build machine's CPU has all of it
# (launcher/forza_launcher/specs.py). The host code keeps the baseline flags and refuses to
# start on a CPU without these features (src/platform/cpu_target.cpp). Adapted from #176
# (FORZA_GUEST_FMA, soconnor0919).
set(FORZA_CPU_TARGET "baseline" CACHE STRING "CPU the generated code is compiled for: baseline or x86-64-v3")
set_property(CACHE FORZA_CPU_TARGET PROPERTY STRINGS baseline x86-64-v3)

function(forza_configure_cpu_target host)
    if(FORZA_CPU_TARGET STREQUAL "baseline")
        return()
    endif()
    if(NOT FORZA_CPU_TARGET STREQUAL "x86-64-v3")
        message(FATAL_ERROR "FORZA_CPU_TARGET must be baseline or x86-64-v3, not '${FORZA_CPU_TARGET}'")
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" OR
       NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        message(FATAL_ERROR "FORZA_CPU_TARGET=x86-64-v3 needs Linux or Windows on x86-64")
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(_flags /clang:-march=x86-64-v3 /clang:-ffp-contract=off)   # clang-cl
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|GNU)$")
        set(_flags -march=x86-64-v3 -ffp-contract=off)
    else()
        message(FATAL_ERROR "FORZA_CPU_TARGET=x86-64-v3 needs clang, clang-cl or GCC")
    endif()
    if(NOT TARGET ${host}_recomp)
        message(FATAL_ERROR "FORZA_CPU_TARGET=x86-64-v3 needs the generated code (run codegen first)")
    endif()
    # Also on each target's precompiled header, which is compiled with the target's options.
    foreach(_target ${host}_recomp ${host}_XMediaFacade_default ${host}_SpeechFacade_default)
        if(TARGET ${_target})
            target_compile_options(${_target} PRIVATE ${_flags})
        endif()
    endforeach()
    target_compile_definitions(${host} PRIVATE FORZA_CPU_TARGET_V3=1)
    message(STATUS "CPU target: x86-64-v3 (the game refuses to start on a CPU without it)")
endfunction()
