# libflute unit tests against RFC 6726 (FLUTE) + 3GPP TS 26.346 (MBMS profile).
# Spec-driven, not implementation-driven: tests encode what the specs say, not
# what the current code does.

add_executable(libflute_tests
  unit/alc_packet_test.cpp
  unit/encoding_symbol_test.cpp
  unit/fdt_test.cpp
  unit/fdt_mbms_test.cpp
  unit/file_partitioning_test.cpp
  unit/file_test.cpp
  unit/direct_receiver_test.cpp
)

target_link_libraries(libflute_tests
  PRIVATE
    flute
    GTest::gtest_main
)

# Disable clang-tidy on the test TUs -- they intentionally exercise edge
# cases (truncated buffers, malformed XML, etc.) that trip clang-tidy
# heuristics geared at production code.
set_target_properties(libflute_tests PROPERTIES CXX_CLANG_TIDY "")

include(GoogleTest)
gtest_discover_tests(libflute_tests)
