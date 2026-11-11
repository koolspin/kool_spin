# Acceptance tests for specs/mixer/spec.md: Catch2 tests of the mixer, which
# call its process() directly. They build on the deck tests' support. The
# golden file test is added after the build loop (see the spec).

ks_add_test(ks_mixer_acceptance_tests
    SOURCES
        listen_test.cpp
        mixer_test.cpp
        realtime_test.cpp
    LIBRARIES ks::engine)
