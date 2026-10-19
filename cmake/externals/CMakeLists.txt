# The libraries Dolphin's disc code needs, built static from vendor/externals.
# zstd, bzip2, liblzma and zlib-ng use Dolphin's own wrapper CMakeLists.txt.

set(DRVZ_EXT "${PROJECT_SOURCE_DIR}/vendor/externals")
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF)
set(SKIP_INSTALL_ALL ON)

# Third-party code: no warnings of ours.
if(MSVC)
  add_compile_options(/W0)
else()
  add_compile_options(-w)
endif()

# zstd (BSD-3-Clause / GPL-2.0)
set(ZSTD_BUILD_PROGRAMS OFF)
set(ZSTD_BUILD_SHARED OFF)
set(ZSTD_BUILD_STATIC ON)
set(ZSTD_BUILD_TESTS OFF)
set(ZSTD_BUILD_CONTRIB OFF)
set(ZSTD_LEGACY_SUPPORT OFF)       # legacy/ is not vendored
set(ZSTD_MULTITHREAD_SUPPORT OFF)  # Dolphin runs its own compression threads
add_subdirectory("${DRVZ_EXT}/zstd" zstd EXCLUDE_FROM_ALL)

# bzip2 (bzip2 license)
add_subdirectory("${DRVZ_EXT}/bzip2" bzip2 EXCLUDE_FROM_ALL)

# liblzma (0BSD / public domain)
add_subdirectory("${DRVZ_EXT}/liblzma" liblzma EXCLUDE_FROM_ALL)

# zlib-ng in zlib-compatible mode (zlib license): GCZ and CRC32
set(WITH_GTEST OFF)
set(WITH_GZFILEOP OFF)
add_subdirectory("${DRVZ_EXT}/zlib-ng" zlib-ng EXCLUDE_FROM_ALL)

# fmt (MIT)
add_library(fmt STATIC
  "${DRVZ_EXT}/fmt/fmt/src/format.cc"
  "${DRVZ_EXT}/fmt/fmt/src/os.cc")
target_include_directories(fmt PUBLIC "${DRVZ_EXT}/fmt/fmt/include")
add_library(fmt::fmt ALIAS fmt)

# mbedtls 2.28 (Apache-2.0): AES, SHA-1, MD5, RSA for Dolphin's crypto code
set(ENABLE_PROGRAMS OFF)
set(ENABLE_TESTING OFF)
set(USE_STATIC_MBEDTLS_LIBRARY ON)
set(USE_SHARED_MBEDTLS_LIBRARY OFF)
set(INSTALL_MBEDTLS_HEADERS OFF)
set(MBEDTLS_FATAL_WARNINGS OFF)
add_subdirectory("${DRVZ_EXT}/mbedtls" mbedtls EXCLUDE_FROM_ALL)
if(NOT TARGET MbedTLS::mbedcrypto)
  add_library(MbedTLS::mbedcrypto ALIAS mbedcrypto)
endif()
target_include_directories(mbedcrypto INTERFACE "${DRVZ_EXT}/mbedtls/include")
