# Acceptance tests for specs/segments/spec.md: Catch2 tests of ks::ipc's
# segments, descriptor passing, runtime directory, and lock files. Tests that
# need several processes fork. Each uses its own temporary runtime directory,
# so they can run in parallel.

ks_add_test(ks_segments_acceptance_tests
    SOURCES
        fd_passing_test.cpp
        runtime_dir_test.cpp
        segment_test.cpp
    LIBRARIES ks::ipc)

# The shm-primitives tests' items, values, and stall watch (test_support.h).
target_include_directories(ks_segments_acceptance_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../shm-primitives)
