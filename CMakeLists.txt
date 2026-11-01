cmake_minimum_required(VERSION 3.22)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(lib-wii-gamecube-kit VERSION 0.1.0
  DESCRIPTION "Dolphin's GameCube/Wii disc image conversion (RVZ, WIA, GCZ, ISO) as a library"
  LANGUAGES C CXX)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vendor/DOLPHIN_COMMIT" WII_GAMECUBE_KIT_DOLPHIN_COMMIT)
string(STRIP "${WII_GAMECUBE_KIT_DOLPHIN_COMMIT}" WII_GAMECUBE_KIT_DOLPHIN_COMMIT)
string(SUBSTRING "${WII_GAMECUBE_KIT_DOLPHIN_COMMIT}" 0 8 WII_GAMECUBE_KIT_DOLPHIN_COMMIT_SHORT)

option(WII_GAMECUBE_KIT_BUILD_CLI "Build the lib-wii-gamecube-kit-cli command line tool" ON)
option(WII_GAMECUBE_KIT_BUILD_TESTS "Build the test suite" ON)

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
# Static CRT, so lib-wii-gamecube-kit.dll needs no VC++ redistributable (as lib-chd-kit).
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ------------------------------------------------------------- architecture
# Dolphin picks code paths (CPU detection, AES/SHA-1 intrinsics) with these.
if(CMAKE_OSX_ARCHITECTURES)
  list(LENGTH CMAKE_OSX_ARCHITECTURES _wii_gamecube_kit_arch_count)
  if(_wii_gamecube_kit_arch_count GREATER 1)
    message(FATAL_ERROR "Build one architecture at a time and merge with lipo (see scripts/build-mac.sh).")
  endif()
  set(_wii_gamecube_kit_arch "${CMAKE_OSX_ARCHITECTURES}")
elseif(CMAKE_GENERATOR_PLATFORM)
  set(_wii_gamecube_kit_arch "${CMAKE_GENERATOR_PLATFORM}")
elseif(MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
  set(_wii_gamecube_kit_arch "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
else()
  set(_wii_gamecube_kit_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(_wii_gamecube_kit_arch MATCHES "^(x64|x86_64|amd64|AMD64)$")
  set(WII_GAMECUBE_KIT_ARCH x86_64)
  set(_wii_gamecube_kit_arch_defs _ARCH_64=1 _M_X86_64=1)
elseif(_wii_gamecube_kit_arch MATCHES "^(arm64|ARM64|aarch64)$")
  set(WII_GAMECUBE_KIT_ARCH arm64)
  set(_wii_gamecube_kit_arch_defs _ARCH_64=1 _M_ARM_64=1)
else()
  message(FATAL_ERROR "Unsupported architecture '${_wii_gamecube_kit_arch}' (x86_64 and arm64 only)")
endif()
message(STATUS "lib-wii-gamecube-kit: Dolphin ${WII_GAMECUBE_KIT_DOLPHIN_COMMIT_SHORT}, ${WII_GAMECUBE_KIT_ARCH}")

# All sources are UTF-8 (fmt requires it on MSVC).
add_compile_options($<$<C_COMPILER_ID:MSVC>:/utf-8>)

# ---------------------------------------------------------------- externals
add_subdirectory(cmake/externals)

# ------------------------------------------------------------ Dolphin code
set(DOLPHIN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dolphin/Source/Core")
file(GLOB_RECURSE _wii_gamecube_kit_dolphin_sources CONFIGURE_DEPENDS "${DOLPHIN_SRC}/*.cpp")
# One CPU detection file per architecture.
list(FILTER _wii_gamecube_kit_dolphin_sources EXCLUDE REGEX "/Common/(x64|Arm|Generic)CPUDetect\\.cpp$")
if(WII_GAMECUBE_KIT_ARCH STREQUAL "x86_64")
  list(APPEND _wii_gamecube_kit_dolphin_sources "${DOLPHIN_SRC}/Common/x64CPUDetect.cpp")
else()
  list(APPEND _wii_gamecube_kit_dolphin_sources "${DOLPHIN_SRC}/Common/ArmCPUDetect.cpp")
endif()
# Windows-only helper (ArmCPUDetect reads the CPU name from the registry).
if(NOT WIN32)
  list(FILTER _wii_gamecube_kit_dolphin_sources EXCLUDE REGEX "/Common/WindowsRegistry\\.cpp$")
endif()

add_library(lib-wii-gamecube-kit-dolphin STATIC
  ${_wii_gamecube_kit_dolphin_sources}
  src/shims/LogManagerShim.cpp)
target_include_directories(lib-wii-gamecube-kit-dolphin
  PUBLIC
    # The shims come first: they stand in for Dolphin headers that are not vendored.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shims"
    "${DOLPHIN_SRC}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(lib-wii-gamecube-kit-dolphin PUBLIC
  WII_GAMECUBE_KIT_BUILD=1
  DATA_DIR=\"\"  # Dolphin's Sys folder; never used by the disc code
  WII_GAMECUBE_KIT_STATIC=1
  ${_wii_gamecube_kit_arch_defs}
  $<$<PLATFORM_ID:Windows>:NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_WARNINGS _SCL_SECURE_NO_WARNINGS
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING>
  $<$<NOT:$<PLATFORM_ID:Windows>>:_FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE>
  $<$<PLATFORM_ID:Linux>:_DEFAULT_SOURCE>)
target_link_libraries(lib-wii-gamecube-kit-dolphin PUBLIC
  fmt::fmt zstd::zstd LibLZMA::LibLZMA BZip2::BZip2 ZLIB::ZLIB MbedTLS::mbedcrypto)
if(MSVC)
  # Dolphin's MSVC settings (its root CMakeLists.txt).
  target_compile_options(lib-wii-gamecube-kit-dolphin PUBLIC
    /Zc:inline /Zc:__cplusplus,enumTypes,externConstexpr,preprocessor,templateScope,throwingNew
    /volatile:iso /fp:precise /Oi /Gy
    /external:anglebrackets /external:W0 /external:templates-)
  target_link_libraries(lib-wii-gamecube-kit-dolphin PUBLIC shlwapi)
else()
  target_compile_options(lib-wii-gamecube-kit-dolphin PRIVATE -Wno-deprecated-declarations)
  find_package(Threads REQUIRED)
  target_link_libraries(lib-wii-gamecube-kit-dolphin PUBLIC Threads::Threads)
endif()
if(ANDROID)
  # Build Dolphin's code as plain POSIX: its ANDROID branches route file access
  # through the Dolphin app's JNI layer (content:// URIs), which isn't here.
  # The NDK toolchain defines ANDROID itself, so take it away again. Bionic's
  # <sys/endian.h> swap16/32/64 macros clash with Common/Swap.h, which only
  # undefines them for ANDROID; lib-wii-gamecube-kit-android.h does that instead.
  target_compile_options(lib-wii-gamecube-kit-dolphin PUBLIC -UANDROID
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/shims/lib-wii-gamecube-kit-android.h")
endif()
if(APPLE)
  # FileUtil uses CoreFoundation for the bundle path; StringUtil uses iconv,
  # which macOS keeps in its own library (glibc has it built in).
  target_link_libraries(lib-wii-gamecube-kit-dolphin PUBLIC "-framework CoreFoundation" iconv)
endif()

# ------------------------------------------------------------- the library
add_library(lib-wii-gamecube-kit SHARED src/lib-wii-gamecube-kit.cpp)
target_link_libraries(lib-wii-gamecube-kit PRIVATE lib-wii-gamecube-kit-dolphin)
target_include_directories(lib-wii-gamecube-kit PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_definitions(lib-wii-gamecube-kit PRIVATE
  WII_GAMECUBE_KIT_BUILDING_SHARED=1
  WII_GAMECUBE_KIT_VERSION="${PROJECT_VERSION}"
  WII_GAMECUBE_KIT_DOLPHIN_COMMIT_SHORT="${WII_GAMECUBE_KIT_DOLPHIN_COMMIT_SHORT}")
# (WII_GAMECUBE_KIT_BUILDING_SHARED wins over the WII_GAMECUBE_KIT_STATIC lib-wii-gamecube-kit-dolphin passes on.)
if(APPLE)
  target_link_options(lib-wii-gamecube-kit PRIVATE
    "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/lib-wii-gamecube-kit.exports")
elseif(UNIX)
  # Static libstdc++ (libc++ on Android): the .so then only needs the build
  # machine's glibc, or Android's libc.
  target_link_options(lib-wii-gamecube-kit PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/lib-wii-gamecube-kit.map" "-Wl,--no-undefined"
    -static-libstdc++ -static-libgcc)
endif()
# lib-wii-gamecube-kit.dll, .so and .dylib on every platform: the name already
# starts with "lib", so no extra prefix.
set_target_properties(lib-wii-gamecube-kit PROPERTIES
  OUTPUT_NAME lib-wii-gamecube-kit PREFIX "" IMPORT_PREFIX "")
if(NOT ANDROID)
  # Android loads lib-wii-gamecube-kit.so by that exact name; no versioned soname there.
  set_target_properties(lib-wii-gamecube-kit PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1)
endif()

# Same code linked statically, for tests that look inside (and for the CLI).
add_library(lib-wii-gamecube-kit-static STATIC src/lib-wii-gamecube-kit.cpp)
set_target_properties(lib-wii-gamecube-kit-static PROPERTIES PREFIX "")
target_link_libraries(lib-wii-gamecube-kit-static PUBLIC lib-wii-gamecube-kit-dolphin)
target_compile_definitions(lib-wii-gamecube-kit-static PRIVATE
  WII_GAMECUBE_KIT_VERSION="${PROJECT_VERSION}"
  WII_GAMECUBE_KIT_DOLPHIN_COMMIT_SHORT="${WII_GAMECUBE_KIT_DOLPHIN_COMMIT_SHORT}")

if(WII_GAMECUBE_KIT_BUILD_CLI)
  add_executable(lib-wii-gamecube-kit-cli cli/lib-wii-gamecube-kit-cli.cpp)
  target_link_libraries(lib-wii-gamecube-kit-cli PRIVATE lib-wii-gamecube-kit-static)
endif()

if(WII_GAMECUBE_KIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
