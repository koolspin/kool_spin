# Acceptance tests for specs/resume-state/spec.md: the CRC, the resume slots'
# layout, snapshots (encoding, validation, and choosing a slot), and the
# engine's writing of snapshots and resuming from them. They use the commands
# and state-out tests' support.

ks_add_test(ks_resume_state_acceptance_tests
    SOURCES
        engine_resume_test.cpp
        layout_test.cpp
        realtime_test.cpp
        snapshot_test.cpp
    LIBRARIES ks::engine)

# The validator's fuzz target, run under the fuzz preset only.
ks_add_fuzz_target(ks_resume_state_fuzz
    SOURCES resume_fuzz.cpp
    RUNS 1000000
    LIBRARIES ks::engine)
