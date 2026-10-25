# nextpi-sup — a stand-in for the NextPi supervisor on the far end of an
# emulated Next UART (inspeccy --uart-pi "nextpi-sup ...").
#
# Standalone: links nothing. POSIX only (fork/exec/select); on Windows it
# builds but reports that it is unsupported.

CMAKE_MINIMUM_REQUIRED(VERSION 3.16)
PROJECT(nextpi-sup CXX)

ADD_EXECUTABLE(nextpi-sup src/main.cpp)
SET_TARGET_PROPERTIES(nextpi-sup PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
IF(NOT MSVC)
    TARGET_COMPILE_OPTIONS(nextpi-sup PRIVATE -O2 -Wall -Wextra)
ENDIF()
INSTALL(TARGETS nextpi-sup RUNTIME DESTINATION bin)
