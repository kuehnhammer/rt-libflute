cmake_minimum_required(VERSION 3.16)

project (libflute VERSION 0.12.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_C_STANDARD_REQUIRED True)


set(CMAKE_CXX_FLAGS_DEBUG_INIT "-Wall -Wextra -Werror")
set(CMAKE_C_FLAGS_DEBUG_INIT "-Wall -Wextra -Werror")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Wall -O3")
set(CMAKE_C_FLAGS_RELEASE_INIT "-Wall -O3")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# libflute is intentionally network-agnostic. The library produces ALC
# packet bytes via Encoder::PacketCallback and consumes them via
# Decoder::feed_packet — wire transport (UDP, multicast, RLC SDUs out
# of the broadcast pipeline, …) is the consumer's responsibility. No
# Boost.asio, no socket abstractions, no PCAP integration in the
# library itself. The example apps in examples/ open plain POSIX
# sockets to demonstrate end-to-end use.
option(LIBFLUTE_BUILD_EXAMPLES "Build the example executables (POSIX-socket transmitter / receiver)" ON)
option(LIBFLUTE_BUILD_TESTING  "Build libflute unit tests (fetches GoogleTest via FetchContent)" ON)
option(LIBFLUTE_BUILD_BENCH    "Build end-to-end round-trip benchmark (transient ~1GB allocations)" OFF)
option(ENABLE_RAPTOR           "Enable support for Raptor FEC" OFF)

find_package(spdlog REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(TINYXML REQUIRED IMPORTED_TARGET tinyxml2)

if(LIBFLUTE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

include_directories(
    "${PROJECT_BINARY_DIR}"
    ${PROJECT_SOURCE_DIR}/include

    SYSTEM
    ${PROJECT_SOURCE_DIR}/utils
    ${SPDLOG_INCLUDEDIR}
    )

configure_file("include/Version.h.in" "Version.h")

set(CMAKE_CXX_CLANG_TIDY clang-tidy)

if(ENABLE_RAPTOR)
  message(STATUS "Compiling bitstem-r10 (lib/raptor/) for Raptor FEC support. To disable support build with -DENABLE_RAPTOR=OFF")
  # bitstem-r10 is a CMake-aware C++ submodule with its own targets.
  # Suppress its tests/benchmarks when embedded -- we only need the
  # `r10` static library.
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(R10_BUILD_BENCH OFF CACHE BOOL "" FORCE)
  add_subdirectory(${PROJECT_SOURCE_DIR}/lib/raptor)
else()
  message(STATUS "Skipping bitstem-r10 build. To enable Raptor FEC support build with -DENABLE_RAPTOR=ON")
endif()

# Single library target. Encoder + Decoder are the public surface; the
# rest are implementation details that remain header-public for
# embedders that need to drive lower-level primitives directly.
add_library(flute "")
target_sources(flute
  PRIVATE
    src/AlcPacket.cpp
    src/Decoder.cpp
    src/Encoder.cpp
    src/EncodingSymbol.cpp
    src/File.cpp
    src/FileDeliveryTable.cpp
    utils/base64.cpp
  PUBLIC
    include/Decoder.h
    include/Encoder.h
    include/File.h
  )

target_include_directories(flute PUBLIC ${PROJECT_SOURCE_DIR}/include/)

if(ENABLE_RAPTOR)
  add_compile_definitions(RAPTOR_ENABLED)
  target_sources(flute
    PRIVATE
      src/fec/RaptorFEC.cpp
    PUBLIC
      include/fec/RaptorFEC.h
  )
  # bitstem-r10's own CMakeLists exports the include path on the `r10`
  # target (target_include_directories(r10 PUBLIC include)), so linking
  # is enough.
  target_link_libraries(flute LINK_PUBLIC r10)
endif()

target_link_libraries(flute
    LINK_PUBLIC
    spdlog::spdlog
    pthread
    crypto
    PkgConfig::TINYXML
)

# ---------------------------------------------------------------------------
# Tests. Disabled when libflute is consumed via add_subdirectory() unless
# the embedder explicitly asks for them with -DLIBFLUTE_BUILD_TESTING=ON.
# ---------------------------------------------------------------------------
if(LIBFLUTE_BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_subdirectory(tests)
endif()

if(LIBFLUTE_BUILD_BENCH)
  add_subdirectory(tests/bench)
endif()
