# Acceptance tests for specs/deck/spec.md: Catch2 tests of the deck and its
# interpolator, rendered offline. golden/ holds the performance's golden file.

ks_add_test(ks_deck_acceptance_tests
    SOURCES
        deck_test.cpp
        interpolation_test.cpp
        listen_test.cpp
        realtime_test.cpp
    LIBRARIES ks::engine)
target_compile_definitions(ks_deck_acceptance_tests PRIVATE
    KS_DECK_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
