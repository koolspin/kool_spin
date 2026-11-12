# Acceptance tests for specs/state-out/spec.md: the engine-state layout, track
# IDs and output latency, the render core's timing, output state, and rendered
# hook, and the engine's publishing of deck, mixer, and status blocks. They use
# the commands tests' clients and tracks.

ks_add_test(ks_state_out_acceptance_tests
    SOURCES
        concurrency_test.cpp
        engine_state_test.cpp
        layout_test.cpp
        realtime_test.cpp
        render_core_test.cpp
    LIBRARIES ks::engine)
