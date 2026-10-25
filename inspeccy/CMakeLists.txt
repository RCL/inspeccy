# inspeccy — the headless CLI tracer. Drives the emulation core under a
# canned input script and writes .btr files. The viewer (../inspeccy-viewer/)
# is the GUI consumer of those files; the two share inspeccy_core + shared but
# do not depend on each other.

CMAKE_MINIMUM_REQUIRED(VERSION 3.16)
PROJECT(inspeccy)
SET(APP_NAME inspeccy)

SET(INSPECCY_SOURCES
    src/args.cpp
    src/asm_cli.cpp
    src/batch.cpp
    src/build_identity.cpp
    src/inspection.cpp
    src/irq_report.cpp
    src/base64.cpp
    src/dump.cpp
    src/main.cpp
    src/replay.cpp
    src/status.cpp
)

ADD_EXECUTABLE(${APP_NAME} ${INSPECCY_SOURCES})

TARGET_INCLUDE_DIRECTORIES(${APP_NAME} PRIVATE src)
TARGET_LINK_LIBRARIES(${APP_NAME} PRIVATE inspeccy_core)

# Runnable artifact -> <prefix>/bin. inspeccy is Qt-free, so a plain
# RUNTIME install is all it needs.
INSTALL(TARGETS ${APP_NAME} RUNTIME DESTINATION bin)
