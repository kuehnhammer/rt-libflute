cmake_minimum_required(VERSION 3.16)

project (libflute VERSION 0.11.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_C_STANDARD_REQUIRED True)


set(CMAKE_CXX_FLAGS_DEBUG_INIT "-Wall -Wextra -Werror")
set(CMAKE_C_FLAGS_DEBUG_INIT "-Wall -Wextra -Werror")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Wall -O3")
set(CMAKE_C_FLAGS_RELEASE_INIT "-Wall -O3")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# When embedded in another project (e.g. 5gbc-rx) only the parser core
# plus DirectReceiver is needed. The socket/pcap/ipsec/transmitter parts
# pull in Boost.asio, libpcap, libnl-3, etc. — gated behind options so
# embedders can opt out.
option(LIBFLUTE_BUILD_SOCKET_RECEIVER "Build the UDP/multicast Receiver class (requires Boost.asio)" ON)
option(LIBFLUTE_BUILD_PCAP_RECEIVER   "Build the PcapReceiver class (requires libpcap and Boost.asio)" ON)
option(LIBFLUTE_BUILD_TRANSMITTER     "Build the Transmitter class (requires Boost.asio)" ON)
option(LIBFLUTE_ENABLE_IPSEC          "Build IPSec ESP key plumbing (requires libnl-3.0)" ON)
option(LIBFLUTE_BUILD_EXAMPLES        "Build the example executables (requires receiver and transmitter)" ON)
option(ENABLE_RAPTOR                  "Enable support for Raptor FEC" OFF)

# Anything Boost.asio-based needs Boost; collapse the three options
# into a single \"do we need Boost?\" predicate for clarity.
set(_libflute_needs_boost OFF)
if(LIBFLUTE_BUILD_SOCKET_RECEIVER OR LIBFLUTE_BUILD_PCAP_RECEIVER OR LIBFLUTE_BUILD_TRANSMITTER)
  set(_libflute_needs_boost ON)
endif()

find_package(spdlog REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(TINYXML REQUIRED IMPORTED_TARGET tinyxml2)

if(_libflute_needs_boost)
  find_package(Boost REQUIRED)
endif()

if(LIBFLUTE_BUILD_PCAP_RECEIVER)
  find_package(PCAP REQUIRED)
endif()

if(LIBFLUTE_ENABLE_IPSEC)
  pkg_check_modules(NETLINK REQUIRED IMPORTED_TARGET libnl-3.0)
endif()

if(LIBFLUTE_BUILD_EXAMPLES)
  if(NOT (LIBFLUTE_BUILD_SOCKET_RECEIVER AND LIBFLUTE_BUILD_TRANSMITTER))
    message(STATUS "libflute: examples require both the socket Receiver and the Transmitter; disabling.")
    set(LIBFLUTE_BUILD_EXAMPLES OFF)
  else()
    add_subdirectory(examples)
  endif()
endif()

include_directories(
    "${PROJECT_BINARY_DIR}"
    ${PROJECT_SOURCE_DIR}/include

    SYSTEM
    ${PROJECT_SOURCE_DIR}/utils
    ${SPDLOG_INCLUDEDIR}
    )
if(LIBFLUTE_BUILD_PCAP_RECEIVER)
  include_directories(SYSTEM ${PCAP_INCLUDEDIR})
endif()
if(LIBFLUTE_ENABLE_IPSEC)
  include_directories(SYSTEM PkgConfig::NETLINK)
endif()

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

# Core: parser + ReceiverBase + File assembly + FDT + DirectReceiver.
# These have no transport dependencies (just spdlog + OpenSSL/MD5 + tinyxml2).
add_library(flute "")
target_sources(flute
  PRIVATE
    src/AlcPacket.cpp src/EncodingSymbol.cpp src/FileDeliveryTable.cpp src/File.cpp
    src/ReceiverBase.cpp
    utils/base64.cpp
  PUBLIC
    include/File.h include/DirectReceiver.h include/ReceiverBase.h
  )

if(LIBFLUTE_BUILD_TRANSMITTER)
  target_sources(flute
    PRIVATE src/Transmitter.cpp
    PUBLIC  include/Transmitter.h
  )
endif()

if(LIBFLUTE_BUILD_SOCKET_RECEIVER)
  target_sources(flute
    PRIVATE src/Receiver.cpp
    PUBLIC  include/Receiver.h
  )
endif()

if(LIBFLUTE_BUILD_PCAP_RECEIVER)
  target_sources(flute
    PRIVATE src/PcapReceiver.cpp
    PUBLIC  include/PcapReceiver.h
  )
endif()

if(LIBFLUTE_ENABLE_IPSEC)
  target_sources(flute
    PRIVATE src/IpSec.cpp
    PUBLIC  include/IpSec.h
  )
endif()

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

if(_libflute_needs_boost)
  target_link_libraries(flute LINK_PUBLIC Boost::boost)
endif()

if(LIBFLUTE_BUILD_PCAP_RECEIVER)
  target_link_libraries(flute LINK_PUBLIC ${PCAP_LIBRARY})
endif()

if(LIBFLUTE_ENABLE_IPSEC)
  target_link_libraries(flute LINK_PUBLIC PkgConfig::NETLINK)
endif()

# zlib was previously linked unconditionally but is unused in the code
# (every ContentEncoding != NONE branch throws). Drop it.
