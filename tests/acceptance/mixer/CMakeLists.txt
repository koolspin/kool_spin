# Acceptance tests for specs/mixer/spec.md: Catch2 tests of the mixer, which
# call its process() directly, or render it offline through a render core.
# They build on the deck tests' support. golden/ holds the crossfade's golden
# file.

ks_add_test(ks_mixer_acceptance_tests
    SOURCES
        listen_test.cpp
        mixer_test.cpp
        realtime_test.cpp
    LIBRARIES ks::engine)
target_compile_definitions(ks_mixer_acceptance_tests PRIVATE
    KS_MIXER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
