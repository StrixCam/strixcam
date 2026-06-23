# cmake/toolchains/jetpack-7.2.cmake
#
# Cross-compile toolchain for NVIDIA Jetson Linux r39.2 (JetPack 7.2).
# Host: x86_64 (Dev Container — ubuntu:24.04 + gcc-13-aarch64-linux-gnu)
# Target: aarch64 (ARM64), Ubuntu 24.04 (noble), glibc 2.39
#
# The NVIDIA jetpack-linux-aarch64-crosscompile-x86 container has no 7.x tag,
# so the sysroot is the L4T r39.2 BSP sample rootfs (tegra-correct) and the
# compiler is the Ubuntu gcc-13 aarch64 cross toolchain (matches noble glibc
# 2.39). Single triple aarch64-linux-gnu — no Bootlin buildroot bridge.

cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------
# Target system identity
# ------------------------------------------------------------
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ------------------------------------------------------------
# Repo-relative paths
# ------------------------------------------------------------
get_filename_component(_TOOLCHAIN_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(_CMAKE_DIR "${_TOOLCHAIN_DIR}" DIRECTORY)
get_filename_component(_REPO_ROOT "${_CMAKE_DIR}" DIRECTORY)

# Docker sets CROSS_SYSROOT=/l4t/targetfs (the unpacked BSP sample rootfs).
# Host default: .cache/sysroot/jetpack-7.2
if(DEFINED ENV{CROSS_SYSROOT})
  set(_SYSROOT "$ENV{CROSS_SYSROOT}")
else()
  set(_SYSROOT "${_REPO_ROOT}/.cache/sysroot/jetpack-7.2")
endif()

# Cross compiler: Ubuntu gcc-13 aarch64 package installs /usr/bin/aarch64-linux-gnu-{gcc,g++}-13.
# Allow an env override (CROSS_GCC_SUFFIX) in case a different gcc series is pinned.
if(DEFINED ENV{CROSS_TRIPLE})
  set(_TRIPLE "$ENV{CROSS_TRIPLE}")
else()
  set(_TRIPLE "aarch64-linux-gnu")
endif()

if(DEFINED ENV{CROSS_GCC_SUFFIX})
  set(_GCCSUF "$ENV{CROSS_GCC_SUFFIX}")
else()
  set(_GCCSUF "-13")
endif()

find_program(_CROSS_CC   NAMES "${_TRIPLE}-gcc${_GCCSUF}" "${_TRIPLE}-gcc"
             DOC "aarch64 cross C compiler")
find_program(_CROSS_CXX  NAMES "${_TRIPLE}-g++${_GCCSUF}" "${_TRIPLE}-g++"
             DOC "aarch64 cross C++ compiler")

if(NOT EXISTS "${_SYSROOT}")
  message(FATAL_ERROR
    "JetPack 7.2 sysroot not found at:\n  ${_SYSROOT}\n"
    "Dev container: Command Palette → 'Dev Containers: Rebuild Container'\n")
endif()

if(NOT _CROSS_CC OR NOT _CROSS_CXX)
  message(FATAL_ERROR
    "aarch64 cross compiler (${_TRIPLE}-gcc${_GCCSUF}) not found on PATH.\n"
    "Install gcc-13-aarch64-linux-gnu / g++-13-aarch64-linux-gnu.\n")
endif()

# ------------------------------------------------------------
# qemu-user emulator (lets ctest and gtest_discover_tests run aarch64
# binaries on the x86_64 build host by routing them through qemu-user
# with -L pointing at the target sysroot's dynamic linker / libs).
# RETAINED from the 6.2 setup — this is the runnable-confidence gate.
# ------------------------------------------------------------
find_program(_QEMU_AARCH64
  NAMES qemu-aarch64-static qemu-aarch64
  DOC "qemu-user emulator for running aarch64 test binaries on the build host")

if(_QEMU_AARCH64)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${_QEMU_AARCH64};-L;${_SYSROOT}"
      CACHE STRING "Emulator used to run cross-compiled binaries (ctest, gtest_discover_tests)")
  message(STATUS "  EMULATOR  : ${CMAKE_CROSSCOMPILING_EMULATOR}")
else()
  message(WARNING
    "qemu-aarch64[-static] not found on PATH — ctest cannot run aarch64 binaries on the build host.\n"
    "Install qemu-user-static or run tests on a real Jetson.")
endif()

# ------------------------------------------------------------
# Compilers
# ------------------------------------------------------------
set(CMAKE_C_COMPILER   "${_CROSS_CC}")
set(CMAKE_CXX_COMPILER "${_CROSS_CXX}")

# ------------------------------------------------------------
# Sysroot
# ------------------------------------------------------------
set(CMAKE_SYSROOT "${_SYSROOT}")

# ------------------------------------------------------------
# CMake find behavior
# ------------------------------------------------------------
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ------------------------------------------------------------
# Library search paths (critical fix) — single aarch64-linux-gnu triple
# ------------------------------------------------------------
set(_SYSROOT_LIB_MULTIARCH_1 "${CMAKE_SYSROOT}/lib/aarch64-linux-gnu")
set(_SYSROOT_LIB_MULTIARCH_2 "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu")
set(_SYSROOT_LIB_USR        "${CMAKE_SYSROOT}/usr/lib")

foreach(_libdir
        "${_SYSROOT_LIB_MULTIARCH_1}"
        "${_SYSROOT_LIB_MULTIARCH_2}"
        "${_SYSROOT_LIB_USR}")
  if(EXISTS "${_libdir}")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT    " -L${_libdir}")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " -L${_libdir}")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT    " -Wl,-rpath-link,${_libdir}")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " -Wl,-rpath-link,${_libdir}")
  endif()
endforeach()

# ------------------------------------------------------------
# pkg-config isolation (very important)
# ------------------------------------------------------------
set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")

set(_PKGCONF_LIBDIR
  "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:"
  "${CMAKE_SYSROOT}/usr/lib/pkgconfig:"
  "${CMAKE_SYSROOT}/usr/share/pkgconfig"
)
string(REPLACE ";" "" _PKGCONF_LIBDIR "${_PKGCONF_LIBDIR}")
set(ENV{PKG_CONFIG_LIBDIR} "${_PKGCONF_LIBDIR}")

# ------------------------------------------------------------
# Avoid embedding host RPATH
# ------------------------------------------------------------
set(CMAKE_SKIP_RPATH TRUE)

# ------------------------------------------------------------
# Debug output
# ------------------------------------------------------------
message(STATUS "JetPack 7.2 (r39.2) toolchain:")
message(STATUS "  REPO_ROOT : ${_REPO_ROOT}")
message(STATUS "  SYSROOT   : ${CMAKE_SYSROOT}")
message(STATUS "  CC        : ${CMAKE_C_COMPILER}")
message(STATUS "  CXX       : ${CMAKE_CXX_COMPILER}")
message(STATUS "  LIB_M1    : ${_SYSROOT_LIB_MULTIARCH_1}")
message(STATUS "  LIB_M2    : ${_SYSROOT_LIB_MULTIARCH_2}")
message(STATUS "  LIB_USR   : ${_SYSROOT_LIB_USR}")
