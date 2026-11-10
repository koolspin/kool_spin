# Acceptance tests for specs/shm-primitives/spec.md: Catch2 tests of ks::ipc.
# The stress tests fail, rather than hang, when nothing makes progress for
# 5 seconds (test_support.h).

ks_add_test(ks_shm_primitives_acceptance_tests
    SOURCES
        layout_test.cpp
        region_test.cpp
        ring_test.cpp
        seqlock_test.cpp
    LIBRARIES ks::ipc)
