cmake_minimum_required(VERSION 3.16)

project (libflute VERSION 0.12.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_C_STANDARD_REQUIRED True)


if(MSVC)
  # _INIT vars only seed the cache on the first configure run; the
  # target-level flags below are what actually pin the discipline.
  # /WX is left off here for the same reason it's off in the parent
  # project on MSVC — see notes in libs/5gbc-rx/CMakeLists.txt.
  set(CMAKE_CXX_FLAGS_DEBUG_INIT "/W4")
  set(CMAKE_C_FLAGS_DEBUG_INIT "/W4")
  set(CMAKE_CXX_FLAGS_RELEASE_INIT "/W4 /O2")
  set(CMAKE_C_FLAGS_RELEASE_INIT "/W4 /O2")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT "/W4 /O2 /Zi")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO_INIT "/W4 /O2 /Zi")
  set(LIBFLUTE_WARNING_FLAGS /W4 /permissive-
      # tinyxml2 / spdlog headers and libflute source itself trigger
      # these on MSVC without indicating real bugs.
      /wd4244 /wd4267 /wd4324 /wd4458 /wd4505 /wd4127)
else()
  set(CMAKE_CXX_FLAGS_DEBUG_INIT "-Wall -Wextra -Wpedantic -Werror")
  set(CMAKE_C_FLAGS_DEBUG_INIT "-Wall -Wextra -Wpedantic -Werror")
  set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Wall -Wextra -Wpedantic -Werror -O3")
  set(CMAKE_C_FLAGS_RELEASE_INIT "-Wall -Wextra -Wpedantic -Werror -O3")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT "-Wall -Wextra -Wpedantic -Werror -O2 -g")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO_INIT "-Wall -Wextra -Wpedantic -Werror -O2 -g")
  set(LIBFLUTE_WARNING_FLAGS -Wall -Wextra -Wpedantic -Werror)
endif()

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
# OpenSSL provides EVP MD5 in src/File.cpp on POSIX. On Windows we use
# the SDK-provided BCrypt API instead — see the _WIN32 branch in
# File.cpp — so the OpenSSL dependency is dropped there.
if(NOT WIN32)
  find_package(OpenSSL REQUIRED)
endif()
# tinyxml2: pkg-config on Linux/macOS where the existing flow works.
# Windows uses find_package against the FetchContent override at the
# parent scope (no pkg-config in the VS toolchain).
if(WIN32)
  find_package(tinyxml2 REQUIRED)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(TINYXML REQUIRED IMPORTED_TARGET tinyxml2)
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
  # bitstem-fec is shipped as a binary distribution under
  # lib/bitstem-fec/<platform>/, version-pinned by lib/bitstem-fec/VERSION.
  # The cmake module below picks the matching tarball, extracts to the
  # build tree, and exposes an imported target `bitstem::fec`. See
  # lib/bitstem-fec/README.md for the version-update workflow.
  #
  # Must run before any add_subdirectory() that creates executables —
  # BitstemFEC.cmake appends to CMAKE_BUILD_RPATH so executables that
  # dlopen libfec.so.0 can resolve it at runtime via the build's
  # bitstem-fec lib dir without LD_LIBRARY_PATH gymnastics.
  include(BitstemFEC)
else()
  message(STATUS "Skipping bitstem-fec integration. To enable Raptor / RaptorQ FEC support build with -DENABLE_RAPTOR=ON")
endif()

if(LIBFLUTE_BUILD_EXAMPLES)
  add_subdirectory(examples)
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
target_compile_options(flute PRIVATE ${LIBFLUTE_WARNING_FLAGS})

if(ENABLE_RAPTOR)
  add_compile_definitions(RAPTOR_ENABLED)
  target_sources(flute
    PRIVATE
      src/fec/FecLoader.cpp
      src/fec/RaptorFEC.cpp
    PUBLIC
      include/fec/FecLoader.h
      include/fec/RaptorFEC.h
  )
  # libfec.so is loaded at runtime via dlopen — libflute has NO link-
  # time dependency on it. Production builds without an FEC license
  # ship without the .so; the loader's available()/has_scheme() probes
  # route Raptor / RaptorQ files through a clean "FEC unavailable"
  # path while CompactNoCode delivery keeps working unchanged.
  #
  # Headers (<bitstem/fec/fec_c.h>, <bitstem/fec/config.h>) come from
  # the imported target's INTERFACE_INCLUDE_DIRECTORIES so we can
  # build against the C ABI signatures without taking the .so as a
  # NEEDED entry. CMAKE_DL_LIBS is `-ldl` on Linux (FreeBSD), empty
  # on macOS / Windows where dlopen is in libc / kernel32.
  # PUBLIC propagation: consumers of libflute that #include
  # <fec/RaptorFEC.h> transitively get <bitstem/fec/fec_c.h> on
  # their include path. No link-time effect — fec_c.h is a forward-
  # declaration / function-pointer-typedef header only.
  target_include_directories(flute PUBLIC
    $<TARGET_PROPERTY:bitstem::fec,INTERFACE_INCLUDE_DIRECTORIES>)
  target_link_libraries(flute LINK_PRIVATE ${CMAKE_DL_LIBS})

  # Examples / tests / bench need libfec.so on the runtime search
  # path so the dlopen succeeds during development; expose the
  # extracted lib directory via BSF_LIB_DIR (set by BitstemFEC.cmake).
  # Production deployments arrange this through their own packaging.
endif()

target_link_libraries(flute LINK_PUBLIC spdlog::spdlog)

if(WIN32)
  # Windows: BCrypt replaces OpenSSL/MD5 in File.cpp; pthread isn't
  # needed (std::thread maps to native threads via the MSVC runtime).
  # tinyxml2 comes from the parent FetchContent override.
  target_link_libraries(flute LINK_PUBLIC tinyxml2::tinyxml2 bcrypt)
else()
  target_link_libraries(flute LINK_PUBLIC
    pthread
    OpenSSL::Crypto
    PkgConfig::TINYXML
  )
endif()

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
