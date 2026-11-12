# Acceptance tests for specs/commands/spec.md: the command slot, the decoder,
# and the engine graph that reads commands and hands tracks and rings to and
# from the audio thread. They build on the mixer and deck tests' support.

ks_add_test(ks_commands_acceptance_tests
    SOURCES
        decoder_test.cpp
        engine_test.cpp
        realtime_test.cpp
    LIBRARIES ks::engine)

# The decoder fuzz target, run under the fuzz preset only.
ks_add_fuzz_target(ks_command_decoder_fuzz
    SOURCES decoder_fuzz.cpp
    RUNS 1000000
    LIBRARIES ks::engine)
