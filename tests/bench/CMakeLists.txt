# End-to-end round-trip benchmark. Built only when explicitly
# enabled — the 500 MB scenarios allocate ~1 GB transiently and the
# Raptor variants take meaningful CPU time.

add_executable(flute_bench flute_bench.cpp)
set_target_properties(flute_bench PROPERTIES CXX_CLANG_TIDY "")
target_link_libraries(flute_bench PRIVATE flute spdlog::spdlog pthread)

if(ENABLE_RAPTOR)
  add_executable(schedule_size schedule_size.cpp)
  set_target_properties(schedule_size PROPERTIES CXX_CLANG_TIDY "")
  target_link_libraries(schedule_size PRIVATE flute r10 pthread)
endif()
