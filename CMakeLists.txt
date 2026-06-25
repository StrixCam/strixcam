cmake_minimum_required(VERSION 3.24)

project(sst_cam_firmware
    VERSION 0.1.0
    LANGUAGES C CXX
)

# ============================================================
# Basics
# ============================================================
set(SST_REPO_ROOT_DIR "${CMAKE_SOURCE_DIR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if (MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

option(SST_ENABLE_TESTS "Enable tests" OFF)

# ============================================================
# Portable dependencies (Conan)
# ============================================================
add_library(sst_cam_firmware_external_libs INTERFACE)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

target_link_libraries(sst_cam_firmware_external_libs INTERFACE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
)

# ============================================================
# System dependencies (JetPack via apt/pkg-config)
# Always enabled for your projects.
# ============================================================
find_package(PkgConfig REQUIRED)

pkg_search_module(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
pkg_search_module(GSTREAMER_SDP REQUIRED IMPORTED_TARGET gstreamer-sdp-1.0>=1.4)
pkg_search_module(GSTREAMER_APP REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
pkg_search_module(GSTREAMER_VIDEO REQUIRED IMPORTED_TARGET gstreamer-video-1.0>=1.4)

target_link_libraries(sst_cam_firmware_external_libs INTERFACE
    PkgConfig::GSTREAMER
    PkgConfig::GSTREAMER_SDP
    PkgConfig::GSTREAMER_APP
    PkgConfig::GSTREAMER_VIDEO
)

pkg_search_module(OPENCV REQUIRED opencv4)

# OpenCV in the JetPack sysroot is mixed: the plain `libopencv_core.so` symlink
# in /usr/lib/aarch64-linux-gnu points at 4.5d, but the imgproc/etc modules
# only exist as 4.8 (SONAME-versioned, no plain `.so`). Linking the plain
# symlinks would pull in two ABI-incompatible cores at runtime. Pin both core
# and imgproc to the matching 4.8 SONAME files explicitly.
find_library(SST_OPENCV_CORE
    NAMES libopencv_core.so.408
    REQUIRED
)
find_library(SST_OPENCV_IMGPROC
    NAMES libopencv_imgproc.so.408
    REQUIRED
)
# imgcodecs (cv::imwrite) for the JPEG thumbnail writer — same 4.8 SONAME pin.
find_library(SST_OPENCV_IMGCODECS
    NAMES libopencv_imgcodecs.so.408
    REQUIRED
)

target_include_directories(sst_cam_firmware_external_libs INTERFACE
    ${OPENCV_INCLUDE_DIRS}
)

target_link_libraries(sst_cam_firmware_external_libs INTERFACE
    ${SST_OPENCV_CORE}
    ${SST_OPENCV_IMGPROC}
    ${SST_OPENCV_IMGCODECS}
)

# Control plane + live preview: BlueZ over D-Bus (sdbus-c++) and the GStreamer
# RTSP server library. Both .debs are extracted into the sysroot by
# .devcontainer/sysroot/003_install_extra_pkgs.sh.
pkg_search_module(SDBUSCPP REQUIRED IMPORTED_TARGET sdbus-c++)
pkg_search_module(GST_RTSP_SERVER REQUIRED IMPORTED_TARGET
                  gstreamer-rtsp-server-1.0>=1.4)

target_link_libraries(sst_cam_firmware_external_libs INTERFACE
    PkgConfig::SDBUSCPP
    PkgConfig::GST_RTSP_SERVER
)

# Overlay rasterizer: Cairo (shapes / rounded-rects / alpha) + Pango (text
# layout). Runtime .so ship in the JetPack base; -dev headers are added by
# 003_install_extra_pkgs.sh. Link directly rather than via pkg-config — pango.pc
# Requires the full X11 .pc chain, none of which a headless RGBA image surface
# needs. glib/gobject come transitively from the GStreamer pkg-config target.
find_path(SST_CAIRO_INCLUDE_DIR NAMES cairo.h PATH_SUFFIXES cairo REQUIRED)
find_path(SST_PANGO_INCLUDE_DIR NAMES pango/pangocairo.h PATH_SUFFIXES pango-1.0 REQUIRED)
# Pango public headers include <hb.h> (HarfBuzz) — it lives in its own dir.
find_path(SST_HARFBUZZ_INCLUDE_DIR NAMES hb.h PATH_SUFFIXES harfbuzz REQUIRED)
find_library(SST_CAIRO_LIB NAMES cairo REQUIRED)
find_library(SST_PANGO_LIB NAMES pango-1.0 REQUIRED)
find_library(SST_PANGOCAIRO_LIB NAMES pangocairo-1.0 REQUIRED)

target_include_directories(sst_cam_firmware_external_libs SYSTEM INTERFACE
    ${SST_CAIRO_INCLUDE_DIR}
    ${SST_PANGO_INCLUDE_DIR}
    ${SST_HARFBUZZ_INCLUDE_DIR}
)
target_link_libraries(sst_cam_firmware_external_libs INTERFACE
    ${SST_PANGOCAIRO_LIB}
    ${SST_PANGO_LIB}
    ${SST_CAIRO_LIB}
)

# ============================================================
# Proto codegen (sst-cam-proto BLE/WiFi wire contract)
# ============================================================
# Cross-compile split (KTD1): protoc runs on the x86_64 build HOST while
# libprotobuf links against the arm64 sysroot. Pin protoc to the host binary
# BEFORE find_package so FindProtobuf does not try to use a target-arch protoc.
# The toolchain sets CMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER, so find_program
# resolves protoc on the host PATH; the library/headers resolve in the sysroot.
find_program(SST_HOST_PROTOC NAMES protoc REQUIRED)
set(Protobuf_PROTOC_EXECUTABLE "${SST_HOST_PROTOC}"
    CACHE FILEPATH "Host protoc for cross-compile codegen" FORCE)

find_package(Protobuf MODULE REQUIRED)

# The imported target advertises the sysroot's /usr/include as an -isystem dir.
# Adding it explicitly shadows GCC's implicit multiarch search and breaks
# <bits/wordsize.h> resolution when cross-compiling. The sysroot already
# provides /usr/include (and google/protobuf/* within it) by default, so drop
# the redundant interface include and keep only the link.
if(TARGET protobuf::libprotobuf)
  set_target_properties(protobuf::libprotobuf PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "")
endif()

set(SST_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
if(NOT EXISTS "${SST_PROTO_DIR}/bluetooth.proto")
  message(FATAL_ERROR
    "proto/ submodule is missing its schema.\n"
    "Run: git submodule update --init --recursive")
endif()

set(SST_PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SST_PROTO_GEN_DIR}")

# Generated .pb.{h,cc} live OUTSIDE src/ so the GLOB_RECURSE src/*.cpp below
# never picks them up. Hand-roll codegen (rather than protobuf_generate) so we
# can pass --experimental_allow_proto3_optional — proto3 `optional` fields
# require it on protoc 3.12.x, and CMake 3.24's protobuf_generate has no option
# to forward extra protoc flags.
set(SST_PROTO_SRCS)
foreach(_proto bluetooth wifi)
  set(_src "${SST_PROTO_GEN_DIR}/${_proto}.pb.cc")
  set(_hdr "${SST_PROTO_GEN_DIR}/${_proto}.pb.h")
  add_custom_command(
      OUTPUT "${_src}" "${_hdr}"
      COMMAND "${Protobuf_PROTOC_EXECUTABLE}"
              --experimental_allow_proto3_optional
              --cpp_out "${SST_PROTO_GEN_DIR}"
              -I "${SST_PROTO_DIR}"
              "${SST_PROTO_DIR}/${_proto}.proto"
      DEPENDS "${SST_PROTO_DIR}/${_proto}.proto"
      COMMENT "protoc (cpp) ${_proto}.proto"
      VERBATIM
  )
  list(APPEND SST_PROTO_SRCS "${_src}")
endforeach()

add_library(sst_proto OBJECT ${SST_PROTO_SRCS})
set_target_properties(sst_proto PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(sst_proto PUBLIC protobuf::libprotobuf)
target_include_directories(sst_proto PUBLIC
    "$<BUILD_INTERFACE:${SST_PROTO_GEN_DIR}>"
)
# The buildroot cross-gcc does not auto-search the Debian multiarch include dir,
# where libc puts arch-specific headers (e.g. <bits/wordsize.h>). Other targets
# get it transitively via pkg-config Cflags; sst_proto has no pkg-config dep, so
# add it explicitly (PRIVATE: only needed to compile the generated code).
target_include_directories(sst_proto SYSTEM PRIVATE
    "${CMAKE_SYSROOT}/usr/include/aarch64-linux-gnu"
)

# Generated protobuf code is not ours — silence the repo's strict warning flags
# (-Wconversion/-Wshadow/-Wpedantic) so it never breaks the build.
target_compile_options(sst_proto PRIVATE -w)

# ============================================================
# Internal library
# ============================================================
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

list(FILTER CORE_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

add_library(sst_cam_firmware_internal_libs STATIC ${CORE_SOURCES})

target_link_libraries(sst_cam_firmware_internal_libs PUBLIC
    sst_cam_firmware_external_libs
    sst_proto
)

target_include_directories(sst_cam_firmware_internal_libs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_compile_definitions(sst_cam_firmware_internal_libs PUBLIC
    SST_REPO_ROOT_DIR="${SST_REPO_ROOT_DIR}"
)

# ============================================================
# Main executable
# ============================================================
add_executable(sst_cam_firmware src/main.cpp)

target_link_libraries(sst_cam_firmware PRIVATE
    sst_cam_firmware_internal_libs
)

# ============================================================
# Tests (Conan-provided GTest if enabled)
# ============================================================
if (SST_ENABLE_TESTS)
    enable_testing()

    find_package(GTest CONFIG REQUIRED)

    file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
    )

    add_executable(sst_cam_firmware_tests ${TEST_SOURCES})

    target_link_libraries(sst_cam_firmware_tests PRIVATE
        sst_cam_firmware_internal_libs
        GTest::gtest
        GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(sst_cam_firmware_tests DISCOVERY_MODE PRE_TEST)
endif()