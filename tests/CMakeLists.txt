include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.8.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(ob_tests
  test_endian.cpp
  test_parser.cpp
  test_mmap.cpp
  test_pool.cpp
  test_id_map.cpp
  test_ref_book.cpp
  test_engine.cpp
  test_flat_book.cpp
  test_differential.cpp
  test_spsc.cpp
  test_pipeline.cpp
  test_match.cpp
)
target_link_libraries(ob_tests PRIVATE ob::lib ob::warnings Catch2::Catch2WithMain)
catch_discover_tests(ob_tests)
