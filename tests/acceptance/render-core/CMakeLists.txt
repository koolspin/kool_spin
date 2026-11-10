# Acceptance tests for specs/render-core/spec.md: Catch2 tests of ks::engine.
# Tests that wait on a device fail, rather than hang, when nothing makes
# progress for 5 seconds (test_support.h).
#
# The hidden test case for the [manual] listening check plays a tone through
# the default device:
#   ks_render_core_acceptance_tests "[listen]"

ks_add_test(ks_render_core_acceptance_tests
    SOURCES
        miniaudio_backend_test.cpp
        offline_test.cpp
        realtime_test.cpp
        render_core_test.cpp
        render_interleaved_test.cpp
    LIBRARIES ks::engine)
