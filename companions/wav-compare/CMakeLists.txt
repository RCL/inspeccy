# wav-compare — spectral and envelope comparison of two WAV files.
#
# A standalone companion: it links nothing, not even inspeccy_shared, so this
# directory can be built or copied on its own.
#
# The work is an FFT per hop plus a great many correlation reductions, which
# is what makes the interpreted version expensive. -ffast-math is deliberately
# NOT used: the exit code is a threshold comparison, so relaxing
# floating-point semantics could flip a
# borderline pass/fail and make the result differ between machines. The
# `#pragma omp simd reduction` clauses buy the vectorisation that GCC would
# otherwise refuse on a floating-point accumulation, with a defined and stable
# association, and -fopenmp-simd enables them without linking an OpenMP
# runtime or starting any threads.

CMAKE_MINIMUM_REQUIRED(VERSION 3.16)
PROJECT(wav-compare CXX)

OPTION(INSPECCY_COMPANION_NATIVE_ARCH
    "Build the companion tools for this exact CPU (not portable)" OFF)

ADD_EXECUTABLE(wav-compare src/main.cpp)
SET_TARGET_PROPERTIES(wav-compare PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

IF(MSVC)
    TARGET_COMPILE_OPTIONS(wav-compare PRIVATE /O2 /fp:precise /openmp:experimental)
ELSE()
    TARGET_COMPILE_OPTIONS(wav-compare PRIVATE
        -O3 -ftree-vectorize -fopenmp-simd -Wall -Wextra)
    IF(INSPECCY_COMPANION_NATIVE_ARCH)
        TARGET_COMPILE_OPTIONS(wav-compare PRIVATE -march=native)
    ENDIF()
ENDIF()

INSTALL(TARGETS wav-compare RUNTIME DESTINATION bin)
