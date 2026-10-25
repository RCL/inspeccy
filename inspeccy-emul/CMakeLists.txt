# inspeccy-emul: a small real-time SDL2 front end over inspeccy_core, for
# playing ZX Spectrum Next .nex programs on a desktop (Linux, Windows) or an
# OpenDingux handheld (RG350M).  It links the core as the CLI and the viewer
# do and changes nothing in it.  Off by default: -DINSPECCY_BUILD_EMUL=ON.
#
# Linux:     system SDL2 (libsdl2-dev)
# Windows:   supply the SDL2 development package through SDL2_DIR;
#            SDL2.dll is copied beside the .exe
# Handheld:  see cmake/opendingux-gcw0.cmake; package with opk/make_opk.sh

ADD_EXECUTABLE(inspeccy-emul
    src/main.cpp
)

TARGET_LINK_LIBRARIES(inspeccy-emul PRIVATE inspeccy_core)

IF(EMSCRIPTEN)
    # SDL2 comes from Emscripten's own ports; the game is preloaded into the
    # virtual file system and the program's files live in IndexedDB at /data.
    SET(INSPECCY_EMUL_WASM_NEX "" CACHE FILEPATH "A .nex preloaded into the page at /game")
    TARGET_COMPILE_OPTIONS(inspeccy-emul PRIVATE -sUSE_SDL=2)
    TARGET_COMPILE_DEFINITIONS(inspeccy-emul PRIVATE INSPECCY_OPENDINGUX=0)
    TARGET_LINK_OPTIONS(inspeccy-emul PRIVATE
        -sUSE_SDL=2
        -lidbfs.js
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=67108864
        -sSTACK_SIZE=1048576
        -sEXIT_RUNTIME=0
        -sFORCE_FILESYSTEM=1
        -sEXPORTED_RUNTIME_METHODS=['callMain','FS','ccall']
        -sINVOKE_RUN=0
        --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)
    IF(INSPECCY_EMUL_WASM_NEX)
        GET_FILENAME_COMPONENT(_emul_nex_name ${INSPECCY_EMUL_WASM_NEX} NAME)
        TARGET_LINK_OPTIONS(inspeccy-emul PRIVATE
            "--preload-file" "${INSPECCY_EMUL_WASM_NEX}@/game/${_emul_nex_name}")
        SET_TARGET_PROPERTIES(inspeccy-emul PROPERTIES LINK_DEPENDS ${INSPECCY_EMUL_WASM_NEX})
    ENDIF()
    SET_TARGET_PROPERTIES(inspeccy-emul PROPERTIES SUFFIX ".html")
    SET_PROPERTY(TARGET inspeccy-emul APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)
ELSEIF(INSPECCY_OPENDINGUX)
    # the sysroot carries SDL2 without a CMake package file: take it straight
    TARGET_INCLUDE_DIRECTORIES(inspeccy-emul PRIVATE ${CMAKE_SYSROOT}/usr/include/SDL2)
    TARGET_COMPILE_DEFINITIONS(inspeccy-emul PRIVATE _REENTRANT INSPECCY_OPENDINGUX=1)
    TARGET_LINK_LIBRARIES(inspeccy-emul PRIVATE SDL2 pthread)
ELSE()
    FIND_PACKAGE(SDL2 REQUIRED)
    TARGET_COMPILE_DEFINITIONS(inspeccy-emul PRIVATE INSPECCY_OPENDINGUX=0)
    IF(WIN32)
        # SDL2main supplies WinMain and calls our main; a windowed (not console) program
        TARGET_LINK_LIBRARIES(inspeccy-emul PRIVATE SDL2::SDL2main SDL2::SDL2)
        SET_TARGET_PROPERTIES(inspeccy-emul PROPERTIES WIN32_EXECUTABLE FALSE)
        ADD_CUSTOM_COMMAND(TARGET inspeccy-emul POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:inspeccy-emul>
            COMMENT "SDL2.dll beside inspeccy-emul.exe")
        INSTALL(FILES $<TARGET_FILE:SDL2::SDL2> DESTINATION bin)
    ELSEIF(TARGET SDL2::SDL2)
        TARGET_LINK_LIBRARIES(inspeccy-emul PRIVATE SDL2::SDL2)
    ELSE()
        TARGET_INCLUDE_DIRECTORIES(inspeccy-emul PRIVATE ${SDL2_INCLUDE_DIRS})
        TARGET_LINK_LIBRARIES(inspeccy-emul PRIVATE ${SDL2_LIBRARIES})
    ENDIF()
ENDIF()

INSTALL(TARGETS inspeccy-emul RUNTIME DESTINATION bin)
