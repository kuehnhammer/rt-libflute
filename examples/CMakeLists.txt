cmake_minimum_required(VERSION 3.16)

# Demo applications. These show how a consumer wires libflute up to
# real network transports — the library has no socket / pcap code of
# its own. Add or remove these freely; they are not part of the
# library's public surface.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_CXX_FLAGS_DEBUG_INIT "-Wall -Wextra -Werror -g3")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Wall -O3")

# Examples lean on the same dependencies the library does plus
# (optionally) libpcap for the pcap-replay receiver.
find_package(spdlog REQUIRED)
find_package(PCAP)

include_directories(
    "${PROJECT_BINARY_DIR}"
    ${PROJECT_SOURCE_DIR}/include

    SYSTEM
    ${SPDLOG_INCLUDEDIR}
    )

# Example apps deliberately exercise edge cases (raw byte buffers,
# argp argument parsing, …) that trip the strict clang-tidy config
# the library uses. Disable clang-tidy on the example TUs.
set(_example_no_tidy "")

add_executable(flute-transmitter flute-transmitter.cpp)
add_executable(flute-receiver    flute-receiver.cpp)
set_target_properties(flute-transmitter flute-receiver
  PROPERTIES CXX_CLANG_TIDY "${_example_no_tidy}")

target_link_libraries(flute-transmitter PRIVATE flute spdlog::spdlog pthread)
target_link_libraries(flute-receiver    PRIVATE flute spdlog::spdlog pthread)

if(PCAP_FOUND)
  add_executable(flute-pcap-receiver flute-pcap-receiver.cpp)
  set_target_properties(flute-pcap-receiver
    PROPERTIES CXX_CLANG_TIDY "${_example_no_tidy}")
  target_include_directories(flute-pcap-receiver SYSTEM PRIVATE ${PCAP_INCLUDEDIR})
  target_link_libraries(flute-pcap-receiver PRIVATE flute spdlog::spdlog pthread ${PCAP_LIBRARY})
else()
  message(STATUS "libpcap not found; skipping flute-pcap-receiver example.")
endif()
