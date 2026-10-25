# Headless emulator, shared trace tools, and optional Qt6 viewer.
# Configure: cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt
# Build:     cmake --build build --config Release
# Test:      ctest --test-dir build -C Release --output-on-failure
# Install:   cmake --install build --config Release --prefix stage

CMAKE_MINIMUM_REQUIRED(VERSION 3.16)
FILE(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" INSPECCY_VERSION LIMIT_COUNT 1)
PROJECT(inspeccy_workspace VERSION "${INSPECCY_VERSION}" LANGUAGES CXX)
SET_PROPERTY(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

SET(CMAKE_CXX_STANDARD 17)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)
SET(CMAKE_CXX_EXTENSIONS OFF)

IF(NOT CMAKE_BUILD_TYPE)
    SET(CMAKE_BUILD_TYPE Release)
ENDIF()

# Stage the compiler runtime beside the installed Windows executables.
IF(MSVC)
    SET(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    INCLUDE(InstallRequiredSystemLibraries)
ENDIF()

IF(MSVC)
    ADD_COMPILE_OPTIONS(/W4 /permissive- /Zc:__cplusplus /utf-8)
    ADD_COMPILE_DEFINITIONS(_CRT_SECURE_NO_WARNINGS)
ELSE()
    ADD_COMPILE_OPTIONS(-Wall -Wextra -Wno-unused-parameter)
ENDIF()

ADD_SUBDIRECTORY(shared)
ADD_SUBDIRECTORY(core)
ADD_SUBDIRECTORY(inspeccy)

# Standalone measurement companions. They link nothing of the above and are
# built here only for convenience; either directory also builds on its own.
ADD_SUBDIRECTORY(companions/ssim-compare)
ADD_SUBDIRECTORY(companions/wav-compare)
ADD_SUBDIRECTORY(companions/nextpi-sup)

# Public Python/CLI adapters accompany an installed headless emulator.
INSTALL(DIRECTORY tools/inspeccy_tools DESTINATION share/inspeccy/tools
    FILES_MATCHING PATTERN "*.py" PATTERN "__pycache__" EXCLUDE)
INSTALL(FILES tools/install_agent_skills.py tools/manual_run.py
    tools/snapshot_periodic.py tools/snapshot_periodic.ps1
    tools/perf_benchmark.py tools/perf_benchmark_all.py tools/perf_benchmark_all.sh tools/README.md
    DESTINATION share/inspeccy/tools)
INSTALL(DIRECTORY docs/ DESTINATION share/inspeccy/docs FILES_MATCHING PATTERN "*.md")
INSTALL(FILES README.md LICENSE VERSION DESTINATION share/inspeccy)
INSTALL(DIRECTORY plugins/inspeccy-skills DESTINATION share/inspeccy/plugins
    FILES_MATCHING PATTERN "*.md" PATTERN "*.json" PATTERN "__pycache__" EXCLUDE)

INCLUDE(CTest)
IF(BUILD_TESTING)
    FIND_PACKAGE(Python3 3.10 COMPONENTS Interpreter)
    ADD_SUBDIRECTORY(tests)
ENDIF()

OPTION(INSPECCY_BUILD_VIEWER
       "Build the Qt6 inspeccy-viewer GUI inspector (requires Qt6 Widgets + Concurrent)" ON)
OPTION(INSPECCY_BUILD_EMUL
       "Build the SDL2 real-time front end inspeccy-emul (desktop Linux and OpenDingux handhelds)" OFF)
IF(INSPECCY_BUILD_EMUL)
    ADD_SUBDIRECTORY(inspeccy-emul)
    IF(BUILD_TESTING AND Python3_Interpreter_FOUND AND NOT EMSCRIPTEN)
        ADD_TEST(NAME emul_copper_frame COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/python/test_emul_beam.py $<TARGET_FILE:inspeccy-emul>)
    ENDIF()
ENDIF()
SET(_inspeccy_viewer_status "skipped (INSPECCY_BUILD_VIEWER=OFF)")
IF(INSPECCY_BUILD_VIEWER)
    # Standard CMake package lookup and an optional Qt SDK root.
    IF(DEFINED ENV{QTDIR})
        LIST(APPEND CMAKE_PREFIX_PATH "$ENV{QTDIR}")
    ENDIF()

    ADD_SUBDIRECTORY(inspeccy-viewer)
    IF(TARGET inspeccy-viewer)
        SET(_inspeccy_viewer_status "yes")
    ELSE()
        SET(_inspeccy_viewer_status "skipped (Qt6 not found)")
    ENDIF()
ENDIF()

MESSAGE(STATUS "inspeccy viewer: ${_inspeccy_viewer_status}")
