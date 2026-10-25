# Shared library — format primitives + small utilities, no Spectrum hardware
# knowledge and no Qt. Consumed by core/, inspeccy/ (CLI), and inspeccy-viewer/.
#
# Public headers:
#   include/trace/*.h    — binary trace format, sld, disasm, text decode
#   src/{sha1,invariants,json_parser,hexdump}.h
#                        — small utilities reachable via the src/ include dir

CONFIGURE_FILE(include/inspeccy/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/inspeccy/version.h @ONLY)

ADD_LIBRARY(inspeccy_shared STATIC
    src/disasm.cpp
    src/sld.cpp
    src/asm_index.cpp
    src/asm_layout.cpp
    src/asm_banking.cpp
    src/text_format.cpp
    src/trace_binary.cpp
    src/trace_decode.cpp
    src/sha1.cpp
    src/json_parser.cpp
    src/hexdump.cpp
    src/frame_path.cpp
    src/output_paths.cpp
)

TARGET_INCLUDE_DIRECTORIES(inspeccy_shared
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
)


