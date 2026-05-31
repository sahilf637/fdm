add_executable(fdm_tests
  doctest_main.cpp
  test_chunk_math.cpp
  test_engine_multi.cpp
  test_chunk_task.cpp
  test_engine_local.cpp
  test_engine_retry.cpp
  test_engine_fallback.cpp
  test_engine_robustness.cpp
  test_engine_disposition.cpp
  test_engine_digest.cpp
  test_engine_headers.cpp
  test_engine_concurrency.cpp
  test_paths.cpp
)
target_include_directories(fdm_tests PRIVATE
  third_party
  ${CMAKE_SOURCE_DIR}/engine/src
)
target_compile_definitions(fdm_tests PRIVATE
  FDM_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(fdm_tests PRIVATE fdm_engine)

# doctest's single-header brings in some -Wpedantic noise; silence it for tests only.
target_compile_options(fdm_tests PRIVATE -Wno-pedantic)

add_test(NAME fdm_tests COMMAND fdm_tests)
