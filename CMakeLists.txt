cmake_minimum_required(VERSION 3.16)

project (libflute VERSION 0.11.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_C_STANDARD_REQUIRED True)

option(ENABLE_RAPTOR10    "Enable support for Raptor10 FEC"     OFF)

set(CMAKE_CXX_FLAGS_DEBUG_INIT "-Wall -Wextra -Werror -g3")
set(CMAKE_C_FLAGS_DEBUG_INIT "-Wall -Wextra -Werror -g3")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Wall -O3")
set(CMAKE_C_FLAGS_RELEASE_INIT "-Wall -O3")

find_package(Boost REQUIRED)
find_package(spdlog REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(TINYXML REQUIRED IMPORTED_TARGET tinyxml2)
pkg_check_modules(NETLINK REQUIRED IMPORTED_TARGET libnl-3.0)

option(ENABLE_RAPTOR "Enable support for Raptor FEC" ON)

add_subdirectory(examples)

include_directories(
    "${PROJECT_BINARY_DIR}"
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/raptor

    SYSTEM
    ${PROJECT_SOURCE_DIR}/utils
    ${SPDLOG_INCLUDEDIR}
    PkgConfig::NETLINK
    )

configure_file("include/Version.h.in" "Version.h")

set(CMAKE_CXX_CLANG_TIDY clang-tidy)

if(ENABLE_RAPTOR)
  message(STATUS "Compiling raptor library for Raptor FEC support. To disable support build with -DENABLE_RAPTOR=OFF")
  add_library(raptor STATIC)
  target_sources(raptor
    PRIVATE
      ${PROJECT_SOURCE_DIR}/lib/raptor/bipartite.c 
      ${PROJECT_SOURCE_DIR}/lib/raptor/decoder.c 
      ${PROJECT_SOURCE_DIR}/lib/raptor/encoder.c 
      ${PROJECT_SOURCE_DIR}/lib/raptor/galois.c 
      ${PROJECT_SOURCE_DIR}/lib/raptor/gaussian.c 
      ${PROJECT_SOURCE_DIR}/lib/raptor/pivoting.c 
      ${PROJECT_SOURCE_DIR}/lib/raptor/random.c
    PUBLIC
      ${PROJECT_SOURCE_DIR}/lib/raptor/raptor.h
  )
  target_link_libraries(raptor LINK_PUBLIC m)
else()
  message(STATUS "Skipping raptor library for Raptor FEC support. To enable support build with -DENABLE_RAPTOR=ON")
endif()

add_library(flute "")
target_sources(flute
  PRIVATE
    src/Receiver.cpp src/Transmitter.cpp src/AlcPacket.cpp src/EncodingSymbol.cpp src/FileDeliveryTable.cpp src/IpSec.cpp src/File.cpp 
    utils/base64.cpp
  PUBLIC
    include/Receiver.h include/Transmitter.h include/File.h

  )
target_include_directories(flute PUBLIC ${PROJECT_SOURCE_DIR}/include/)

if(ENABLE_RAPTOR)
  add_compile_definitions(RAPTOR_ENABLED)
  target_include_directories(flute PUBLIC ${PROJECT_SOURCE_DIR}/lib/raptor/)
  target_sources(flute
    PRIVATE
	  src/fec/RaptorFEC.cpp
	PUBLIC
	  include/fec/RaptorFEC.h
  )
  target_link_libraries(flute LINK_PUBLIC raptor)
endif()

target_link_libraries( flute
    LINK_PUBLIC
    spdlog::spdlog
    z
    pthread
    crypto
    PkgConfig::TINYXML
    PkgConfig::NETLINK
)
