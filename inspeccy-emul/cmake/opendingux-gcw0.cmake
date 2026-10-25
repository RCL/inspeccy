# CMake toolchain file for the OpenDingux beta (RG350/RG350M, GCW Zero and
# kin: Ingenic JZ4770, MIPS32r2 little-endian, uClibc, SDL2 in the sysroot).
# Toolchain: opendingux-gcw0-toolchain (od-2022.09.22 or later) unpacked at
# /opt/gcw0-toolchain, or point OPENDINGUX_TOOLCHAIN at it.
#
#   cmake -S . -B build-gcw0 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=inspeccy-emul/cmake/opendingux-gcw0.cmake \
#         -DINSPECCY_BUILD_EMUL=ON -DINSPECCY_BUILD_VIEWER=OFF -DBUILD_TESTING=OFF

SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_SYSTEM_PROCESSOR mipsel)

IF(DEFINED ENV{OPENDINGUX_TOOLCHAIN})
    SET(OPENDINGUX_TOOLCHAIN "$ENV{OPENDINGUX_TOOLCHAIN}")
ELSEIF(NOT DEFINED OPENDINGUX_TOOLCHAIN)
    SET(OPENDINGUX_TOOLCHAIN /opt/gcw0-toolchain)
ENDIF()

SET(CMAKE_SYSROOT ${OPENDINGUX_TOOLCHAIN}/mipsel-gcw0-linux-uclibc/sysroot)
SET(CMAKE_C_COMPILER   ${OPENDINGUX_TOOLCHAIN}/usr/bin/mipsel-linux-gcc)
SET(CMAKE_CXX_COMPILER ${OPENDINGUX_TOOLCHAIN}/usr/bin/mipsel-linux-g++)

SET(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The JZ4770: MIPS32r2 with the XBurst FPU; the beta's own packages use these.
SET(OPENDINGUX_ARCH_FLAGS "-march=mips32r2 -mtune=mips32r2 -mhard-float -mno-mips16 -mno-micromips")
SET(CMAKE_C_FLAGS_INIT   "${OPENDINGUX_ARCH_FLAGS}")
SET(CMAKE_CXX_FLAGS_INIT "${OPENDINGUX_ARCH_FLAGS}")
SET(CMAKE_C_FLAGS_RELEASE_INIT   "-O2 -DNDEBUG")
SET(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
# the firmware's own libstdc++ may be older than this toolchain's: carry ours
SET(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")

SET(INSPECCY_OPENDINGUX ON CACHE BOOL "Building for an OpenDingux handheld" FORCE)
