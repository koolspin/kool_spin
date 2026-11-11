# Acceptance tests for specs/safety-stage/spec.md: Catch2 tests of the output
# safety stage, through ks::engine's render core.

ks_add_test(ks_safety_stage_acceptance_tests
    SOURCES
        realtime_test.cpp
        safety_stage_test.cpp
    LIBRARIES ks::engine)
