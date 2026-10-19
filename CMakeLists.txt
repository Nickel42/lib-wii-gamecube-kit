cmake_minimum_required(VERSION 3.25)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(libdolphinrvz VERSION 0.1.0
  DESCRIPTION "Dolphin's GameCube/Wii disc image conversion (RVZ, WIA, GCZ, ISO) as a library"
  LANGUAGES C CXX)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vendor/DOLPHIN_COMMIT" DRVZ_DOLPHIN_COMMIT)
string(STRIP "${DRVZ_DOLPHIN_COMMIT}" DRVZ_DOLPHIN_COMMIT)
string(SUBSTRING "${DRVZ_DOLPHIN_COMMIT}" 0 8 DRVZ_DOLPHIN_COMMIT_SHORT)

option(DRVZ_BUILD_CLI "Build the drvz-cli command line tool" ON)
option(DRVZ_BUILD_TESTS "Build the test suite" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# Static CRT, so dolphinrvz.dll needs no VC++ redistributable (as libchdman).
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ------------------------------------------------------------- architecture
# Dolphin picks code paths (CPU detection, AES/SHA-1 intrinsics) with these.
if(CMAKE_OSX_ARCHITECTURES)
  list(LENGTH CMAKE_OSX_ARCHITECTURES _drvz_arch_count)
  if(_drvz_arch_count GREATER 1)
    message(FATAL_ERROR "Build one architecture at a time and merge with lipo (see scripts/build-mac.sh).")
  endif()
  set(_drvz_arch "${CMAKE_OSX_ARCHITECTURES}")
elseif(CMAKE_GENERATOR_PLATFORM)
  set(_drvz_arch "${CMAKE_GENERATOR_PLATFORM}")
elseif(MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
  set(_drvz_arch "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
else()
  set(_drvz_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(_drvz_arch MATCHES "^(x64|x86_64|amd64|AMD64)$")
  set(DRVZ_ARCH x86_64)
  set(_drvz_arch_defs _ARCH_64=1 _M_X86_64=1)
elseif(_drvz_arch MATCHES "^(arm64|ARM64|aarch64)$")
  set(DRVZ_ARCH arm64)
  set(_drvz_arch_defs _ARCH_64=1 _M_ARM_64=1)
else()
  message(FATAL_ERROR "Unsupported architecture '${_drvz_arch}' (x86_64 and arm64 only)")
endif()
message(STATUS "libdolphinrvz: Dolphin ${DRVZ_DOLPHIN_COMMIT_SHORT}, ${DRVZ_ARCH}")

# All sources are UTF-8 (fmt requires it on MSVC).
add_compile_options($<$<C_COMPILER_ID:MSVC>:/utf-8>)

# ---------------------------------------------------------------- externals
add_subdirectory(cmake/externals)

# ------------------------------------------------------------ Dolphin code
set(DOLPHIN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core")
file(GLOB_RECURSE _drvz_dolphin_sources CONFIGURE_DEPENDS "${DOLPHIN_SRC}/*.cpp")
# One CPU detection file per architecture.
list(FILTER _drvz_dolphin_sources EXCLUDE REGEX "/Common/(x64|Arm|Generic)CPUDetect\\.cpp$")
if(DRVZ_ARCH STREQUAL "x86_64")
  list(APPEND _drvz_dolphin_sources "${DOLPHIN_SRC}/Common/x64CPUDetect.cpp")
else()
  list(APPEND _drvz_dolphin_sources "${DOLPHIN_SRC}/Common/ArmCPUDetect.cpp")
endif()

add_library(drvz_dolphin STATIC
  ${_drvz_dolphin_sources}
  src/shims/LogManagerShim.cpp)
target_include_directories(drvz_dolphin
  PUBLIC
    # The shims come first: they stand in for Dolphin headers that are not vendored.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shims"
    "${DOLPHIN_SRC}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(drvz_dolphin PUBLIC
  DRVZ_BUILD=1
  DATA_DIR=\"\"  # Dolphin's Sys folder; never used by the disc code
  DRVZ_STATIC=1
  ${_drvz_arch_defs}
  $<$<PLATFORM_ID:Windows>:NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_WARNINGS _SCL_SECURE_NO_WARNINGS
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING>
  $<$<NOT:$<PLATFORM_ID:Windows>>:_FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE>
  $<$<PLATFORM_ID:Linux>:_DEFAULT_SOURCE>)
target_link_libraries(drvz_dolphin PUBLIC
  fmt::fmt zstd::zstd LibLZMA::LibLZMA BZip2::BZip2 ZLIB::ZLIB MbedTLS::mbedcrypto)
if(MSVC)
  # Dolphin's MSVC settings (its root CMakeLists.txt).
  target_compile_options(drvz_dolphin PUBLIC
    /Zc:inline /Zc:__cplusplus,enumTypes,externConstexpr,preprocessor,templateScope,throwingNew
    /volatile:iso /fp:precise /Oi /Gy
    /external:anglebrackets /external:W0 /external:templates-)
  target_link_libraries(drvz_dolphin PUBLIC shlwapi)
else()
  target_compile_options(drvz_dolphin PRIVATE -Wno-deprecated-declarations)
  find_package(Threads REQUIRED)
  target_link_libraries(drvz_dolphin PUBLIC Threads::Threads)
endif()

# ------------------------------------------------------------- the library
add_library(dolphinrvz SHARED src/drvz_capi.cpp)
target_link_libraries(dolphinrvz PRIVATE drvz_dolphin)
target_include_directories(dolphinrvz PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_definitions(dolphinrvz PRIVATE
  DRVZ_BUILDING_SHARED=1
  DRVZ_VERSION="${PROJECT_VERSION}"
  DRVZ_DOLPHIN_COMMIT_SHORT="${DRVZ_DOLPHIN_COMMIT_SHORT}")
# (DRVZ_BUILDING_SHARED wins over the DRVZ_STATIC drvz_dolphin passes on.)
if(APPLE)
  target_link_options(dolphinrvz PRIVATE
    "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/dolphinrvz.exports")
elseif(UNIX)
  target_link_options(dolphinrvz PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/dolphinrvz.map" "-Wl,--no-undefined")
endif()
set_target_properties(dolphinrvz PROPERTIES
  OUTPUT_NAME dolphinrvz
  VERSION ${PROJECT_VERSION}
  SOVERSION 1)

# Same code linked statically, for tests that look inside (and for the CLI).
add_library(dolphinrvz_static STATIC src/drvz_capi.cpp)
target_link_libraries(dolphinrvz_static PUBLIC drvz_dolphin)
target_compile_definitions(dolphinrvz_static PRIVATE
  DRVZ_VERSION="${PROJECT_VERSION}"
  DRVZ_DOLPHIN_COMMIT_SHORT="${DRVZ_DOLPHIN_COMMIT_SHORT}")

if(DRVZ_BUILD_CLI)
  add_executable(drvz-cli cli/drvz_cli.cpp)
  target_link_libraries(drvz-cli PRIVATE dolphinrvz_static)
endif()

if(DRVZ_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
