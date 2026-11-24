# SPDX-License-Identifier: GPL-3.0-only
# Cross build of the FreeBSD archive with clang and lld from another system.
#
#   cmake -S . -B build/freebsd-amd64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/FreeBSD-cross.cmake \
#       -DFREEBSD_SYSROOT=/path/to/sysroot -DFREEBSD_ARCH=amd64 \
#       -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DLUNACORE_BUILD_FREEBSD_APP=ON
#   cmake --build build/freebsd-amd64
#   cpack --config build/freebsd-amd64/CPackConfig.cmake -B build/freebsd-amd64
#
# The sysroot is base.txz of the oldest supported FreeBSD release, with the curl, gtk3,
# ffmpeg and sdl2 packages and their dependencies extracted on top. The program uses the
# system's libc++, which only gains functions over time, so it must be built against the
# oldest libc++ it will meet. FreeBSD 14.5 ships libc++ 21 while 14.4, 15.0 and 15.1 ship
# libc++ 19. A build on 14.5 stops on 15.1 with
# 'Undefined symbol "_ZNSt3__113__hash_memoryEPKvm"'. A build on 14.4 runs on all four.

set(FREEBSD_ARCH "amd64" CACHE STRING "FreeBSD architecture: amd64 or aarch64")
set(FREEBSD_RELEASE "14.4" CACHE STRING "FreeBSD release of the sysroot")
set(FREEBSD_SYSROOT "" CACHE PATH "FreeBSD sysroot: base.txz and the packages")
# try_compile projects read this file again and need the same values.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES FREEBSD_ARCH FREEBSD_RELEASE FREEBSD_SYSROOT)

if(NOT EXISTS "${FREEBSD_SYSROOT}/usr/include/c++/v1/__config")
    message(FATAL_ERROR "Set FREEBSD_SYSROOT to a FreeBSD sysroot (base.txz and packages).")
endif()
if(FREEBSD_ARCH STREQUAL "amd64")
    set(freebsd_cpu x86_64)
elseif(FREEBSD_ARCH STREQUAL "aarch64")
    set(freebsd_cpu aarch64)
else()
    message(FATAL_ERROR "FREEBSD_ARCH must be amd64 or aarch64.")
endif()

set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_VERSION ${FREEBSD_RELEASE})
# Names the archive: LunaONE-V<version>-FreeBSD-<arch>.tar.gz.
set(CMAKE_SYSTEM_PROCESSOR ${FREEBSD_ARCH})
set(CMAKE_SYSROOT "${FREEBSD_SYSROOT}")

# clang takes libc++'s headers from the sysroot, so they match the system's libc++.
set(freebsd_target ${freebsd_cpu}-unknown-freebsd${FREEBSD_RELEASE})
set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET ${freebsd_target})
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_COMPILER_TARGET ${freebsd_target})
foreach(kind EXE SHARED MODULE)
    set(CMAKE_${kind}_LINKER_FLAGS_INIT "-fuse-ld=lld")
endforeach()
find_program(CMAKE_STRIP NAMES llvm-strip)

set(CMAKE_FIND_ROOT_PATH "${FREEBSD_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# GTK, FFmpeg and SDL2 come from the sysroot's packages, in pkgconf's FreeBSD search path.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${FREEBSD_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${FREEBSD_SYSROOT}/usr/libdata/pkgconfig:${FREEBSD_SYSROOT}/usr/local/libdata/pkgconfig:${FREEBSD_SYSROOT}/usr/local/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
