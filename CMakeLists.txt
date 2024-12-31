cmake_minimum_required(VERSION 3.14)

option(BUILD_DEPENDENCY "Build dependencies" ON)
option(BUILD_GRPC "Build with grpc support" ON)
option(BUILD_SHARED_LIBS "Build core as shared libraries instead of static ones" OFF)
option(BUILD_EXAMPLE "Build example" OFF)
option(BUILD_QCOM "Build qualcomm platform" OFF)
option(BUILD_MAVSDK "Build mavsdk dependency" ON)

project(MAVCam)

# manually change version
set(VERSION_STR "v0.8.0")
message(STATUS "Version: ${VERSION_STR}")
STRING(REGEX REPLACE v\([0-9]+\).[0-9]+.[0-9]+.*$ \\1 VERSION_MAJOR "${VERSION_STR}")
STRING(REGEX REPLACE v[0-9]+.\([0-9]+\).[0-9]+.*$ \\1 VERSION_MINOR "${VERSION_STR}")
STRING(REGEX REPLACE v[0-9]+.[0-9]+.\([0-9]+\).*$ \\1 VERSION_PATCH "${VERSION_STR}")

message(STATUS "Version major: ${VERSION_MAJOR}")
message(STATUS "Version minor: ${VERSION_MINOR}")
message(STATUS "Version patch: ${VERSION_PATCH}")

set(BUILD_TIME "")
string(TIMESTAMP BUILD_TIME "%Y-%m-%dT%H:%M:%S")

set(VERSION_STRING ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH})

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/version.h
  @ONLY)
include_directories(${PROJECT_BINARY_DIR})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(DEP_BUILD_DIR "${PROJECT_BINARY_DIR}/third_party" CACHE PATH "Install path for the dependencies. Ignored if SUPERBUILD=OFF.")
set(DEP_INSTALL_DIR "${DEP_BUILD_DIR}/install" CACHE PATH "Install path for the dependencies. Ignored if SUPERBUILD=OFF.")
set(DEP_DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/download" CACHE PATH "Download path for the dependencies. Ignored if SUPERBUILD=OFF.")
set(STAGING_DIR "${PROJECT_SOURCE_DIR}/stage_dir" CACHE PATH "Install path")

include(GNUInstallDirs)

add_subdirectory(third_party)

add_subdirectory(src)

if (BUILD_EXAMPLE)
    add_subdirectory(example)
endif()
