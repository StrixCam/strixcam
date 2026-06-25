# cmake/toolchains/jetson-r36.5.cmake
#
# Cross-compile toolchain for NVIDIA Jetson Linux r36.5 (JetPack 6.x)
# Host: x86_64 (Dev Container — nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:6.1)
# Target: aarch64 (ARM64)

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

# Allow env-var overrides so the Docker image can point to its baked-in paths.
# Docker sets: CROSS_SYSROOT=/l4t/targetfs
#              BOOTLIN_TOOLCHAIN_BIN=/l4t/toolchain/aarch64--glibc--stable-2022.08-1/bin
# Host default: .cache/sysroot/jetson-r36.5 and .cache/toolchains/bootlin-jetson-r36/bin
if(DEFINED ENV{CROSS_SYSROOT})
  set(_SYSROOT "$ENV{CROSS_SYSROOT}")
else()
  set(_SYSROOT "${_REPO_ROOT}/.cache/sysroot/jetson-r36.5")
endif()

if(DEFINED ENV{BOOTLIN_TOOLCHAIN_BIN})
  set(_BOOTLIN_BIN "$ENV{BOOTLIN_TOOLCHAIN_BIN}")
else()
  set(_BOOTLIN_BIN "${_REPO_ROOT}/.cache/toolchains/bootlin-jetson-r36/bin")
endif()

if(NOT EXISTS "${_SYSROOT}")
  message(FATAL_ERROR
    "Jetson sysroot not found at:\n  ${_SYSROOT}\n"
    "Dev container: Command Palette → 'Dev Containers: Rebuild Container'\n")
endif()

if(NOT EXISTS "${_BOOTLIN_BIN}/aarch64-buildroot-linux-gnu-gcc")
  message(FATAL_ERROR
    "Bootlin cross compiler not found at:\n  ${_BOOTLIN_BIN}\n"
    "Dev container: Command Palette → 'Dev Containers: Rebuild Container'\n")
endif()

# ------------------------------------------------------------
# qemu-user emulator (lets ctest and gtest_discover_tests run aarch64
# binaries on the x86_64 build host by routing them through qemu-user
# with -L pointing at the target sysroot's dynamic linker / libs).
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
set(CMAKE_C_COMPILER   "${_BOOTLIN_BIN}/aarch64-buildroot-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${_BOOTLIN_BIN}/aarch64-buildroot-linux-gnu-g++")

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
# Library search paths (critical fix)
# ------------------------------------------------------------
set(_SYSROOT_LIB_MULTIARCH_1 "${CMAKE_SYSROOT}/lib/aarch64-linux-gnu")
set(_SYSROOT_LIB_MULTIARCH_2 "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu")
set(_SYSROOT_LIB_USR        "${CMAKE_SYSROOT}/usr/lib")

# Always add all possible lib locations
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

# Optional: force cross pkg-config if desired
# set(PKG_CONFIG_EXECUTABLE "${_BOOTLIN_BIN}/pkg-config")

# ------------------------------------------------------------
# Avoid embedding host RPATH
# ------------------------------------------------------------
set(CMAKE_SKIP_RPATH TRUE)

# ------------------------------------------------------------
# Debug output
# ------------------------------------------------------------
message(STATUS "Jetson r36.5 toolchain:")
message(STATUS "  REPO_ROOT : ${_REPO_ROOT}")
message(STATUS "  SYSROOT   : ${CMAKE_SYSROOT}")
message(STATUS "  CC        : ${CMAKE_C_COMPILER}")
message(STATUS "  CXX       : ${CMAKE_CXX_COMPILER}")
message(STATUS "  LIB_M1    : ${_SYSROOT_LIB_MULTIARCH_1}")
message(STATUS "  LIB_M2    : ${_SYSROOT_LIB_MULTIARCH_2}")
message(STATUS "  LIB_USR   : ${_SYSROOT_LIB_USR}")