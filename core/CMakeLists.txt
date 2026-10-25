# Z80 and machine emulation, without Qt or CLI dependencies.

OPTION(INSPECCY_TRACE_SUPPORT "Compile in per-instruction trace support" ON)

SET(CORE_SOURCES
    src/z80.cpp
    src/memory.cpp
    src/diagnostics.cpp
    src/video.cpp
    src/metrics.cpp
    src/psg.cpp
    src/sprites.cpp
    src/tilemap.cpp
    src/dma.cpp
    src/copper.cpp
    src/rom.cpp
    src/sna.cpp
    src/z80snap.cpp
    src/tape.cpp
    src/autotype.cpp
    src/dck.cpp
    src/nex.cpp
    src/nextzxos.cpp
    src/dotcmd.cpp
    src/trdos.cpp
    src/audio_render.cpp
    src/peephole.cpp
    src/input.cpp
    src/invariants.cpp
    src/bmp.cpp
    src/uart.cpp
)

IF(INSPECCY_TRACE_SUPPORT)
    LIST(APPEND CORE_SOURCES src/trace.cpp)
ENDIF()

ADD_LIBRARY(inspeccy_core STATIC ${CORE_SOURCES})

TARGET_INCLUDE_DIRECTORIES(inspeccy_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)

TARGET_LINK_LIBRARIES(inspeccy_core PUBLIC inspeccy_shared)

IF(INSPECCY_TRACE_SUPPORT)
    TARGET_COMPILE_DEFINITIONS(inspeccy_core PUBLIC INSPECCY_TRACE_SUPPORT=1)
    FIND_PACKAGE(Threads REQUIRED)
    TARGET_LINK_LIBRARIES(inspeccy_core PUBLIC Threads::Threads)
ELSE()
    TARGET_COMPILE_DEFINITIONS(inspeccy_core PUBLIC INSPECCY_TRACE_SUPPORT=0)
ENDIF()
