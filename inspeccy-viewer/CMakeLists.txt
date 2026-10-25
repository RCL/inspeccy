# inspeccy-viewer — Qt6 GUI inspector. Reads .btr trace files; will gain
# interactive emulation. Links inspeccy_shared (format + utilities). Today it
# does not depend on inspeccy_core; once interactive mode lands, it will link
# core too.

FIND_PACKAGE(Qt6 6.2 COMPONENTS Widgets Concurrent QUIET)
IF(NOT Qt6_FOUND)
    MESSAGE(STATUS "Qt6 not found — skipping inspeccy-viewer. "
                   "Install qt6-base-dev (or set CMAKE_PREFIX_PATH to a Qt6 SDK).")
    RETURN()
ENDIF()

SET(CMAKE_AUTOMOC ON)
SET(CMAKE_AUTORCC ON)
SET(CMAKE_AUTOUIC ON)

ADD_EXECUTABLE(inspeccy-viewer
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/OffscreenFonts.h
    src/InterruptView.cpp
    src/InterruptView.h
    src/TraceModel.cpp
    src/TraceModel.h
    src/RecordWalker.cpp
    src/RecordWalker.h
    src/CallGraphBuilder.cpp
    src/CallGraphBuilder.h
    src/TraceLaunchDialog.cpp
    src/TraceLaunchDialog.h
    src/TraceLoadProgressDialog.cpp
    src/TraceLoadProgressDialog.h
    src/OverviewWidget.cpp
    src/OverviewWidget.h
    # RegisterView retired in Batch 1 (Jun 2026) — CPU state now lives in
    # the icicle ruler. Source files retained in svn for one release in case
    # the layout regresses.
    src/IcicleWidget.cpp
    src/IcicleWidget.h
    src/InstructionView.cpp
    src/InstructionView.h
    src/MemoryView.cpp
    src/MemoryView.h
    src/Cursor.h
)

TARGET_LINK_LIBRARIES(inspeccy-viewer
    PRIVATE
        inspeccy_shared
        Qt6::Widgets
        Qt6::Concurrent
)

# Install the viewer executable into <prefix>/bin alongside inspeccy.
IF(BUILD_TESTING)
    ADD_EXECUTABLE(inspeccy_test_viewer_interrupts
        ../tests/cpp/test_viewer_interrupts.cpp
        src/CallGraphBuilder.cpp src/TraceModel.cpp src/RecordWalker.cpp
        src/InterruptView.cpp src/IcicleWidget.cpp src/TraceLaunchDialog.cpp)
    TARGET_INCLUDE_DIRECTORIES(inspeccy_test_viewer_interrupts PRIVATE src)
    TARGET_LINK_LIBRARIES(inspeccy_test_viewer_interrupts PRIVATE inspeccy_shared Qt6::Widgets)
    ADD_TEST(NAME viewer_interrupts COMMAND inspeccy_test_viewer_interrupts -platform offscreen)

    # Opt-in end-to-end UI capture. Uses Qt's offscreen platform, real dialog
    # buttons, menu actions and keyboard events; never controls the desktop.
    GET_TARGET_PROPERTY(_viewer_ui_sources inspeccy-viewer SOURCES)
    LIST(REMOVE_ITEM _viewer_ui_sources src/main.cpp)
    ADD_EXECUTABLE(inspeccy_viewer_usability EXCLUDE_FROM_ALL
        ../tests/cpp/viewer_usability_capture.cpp ${_viewer_ui_sources})
    TARGET_INCLUDE_DIRECTORIES(inspeccy_viewer_usability PRIVATE src)
    TARGET_LINK_LIBRARIES(inspeccy_viewer_usability PRIVATE inspeccy_shared Qt6::Widgets Qt6::Concurrent)
ENDIF()

INSTALL(TARGETS inspeccy-viewer RUNTIME DESTINATION bin)

# A Qt GUI exe can't run without its Qt runtime beside it. Locate windeployqt
# (ships in the Qt bin dir next to qmake) and run it at install time against
# the freshly-installed exe, so <prefix>/bin becomes self-contained: Qt6*.dll,
# platforms/qwindows.dll, styles/, tls/, imageformats/, etc. Without this the
# installed inspeccy-viewer.exe would fail to start with a missing-Qt6Core.dll error.
IF(WIN32)
    IF(TARGET Qt6::qmake)
        GET_TARGET_PROPERTY(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
        GET_FILENAME_COMPONENT(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
    ENDIF()
    FIND_PROGRAM(WINDEPLOYQT_EXECUTABLE NAMES windeployqt HINTS "${_qt_bin_dir}")
    IF(WINDEPLOYQT_EXECUTABLE)
        INSTALL(CODE "
            MESSAGE(STATUS \"Deploying Qt runtime into \${CMAKE_INSTALL_PREFIX}/bin\")
            EXECUTE_PROCESS(COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                --no-translations --no-opengl-sw
                \"\${CMAKE_INSTALL_PREFIX}/bin/inspeccy-viewer.exe\"
                RESULT_VARIABLE _wdq_rc)
            IF(_wdq_rc)
                MESSAGE(FATAL_ERROR \"windeployqt failed (exit \${_wdq_rc})\")
            ENDIF()
        ")
    ELSE()
        MESSAGE(WARNING "windeployqt not found next to qmake (${_qt_bin_dir}); "
                        "the installed inspeccy-viewer.exe will need Qt DLLs on PATH to run.")
    ENDIF()
ENDIF()
