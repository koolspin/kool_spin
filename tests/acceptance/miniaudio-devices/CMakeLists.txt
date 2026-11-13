# Acceptance tests for specs/miniaudio-devices/spec.md: Catch2 tests of
# ks::engine's miniaudio backends, device listing, loss detector, and device
# source, on miniaudio's null backend. Tests that wait for something fail,
# rather than hang, when it hasn't happened within the stated time. The
# hidden "[cable]" test is for the manual cable-pull criteria.

ks_add_test(ks_miniaudio_devices_acceptance_tests
    SOURCES
        backends_test.cpp
        cable_test.cpp
        loss_detector_test.cpp
        realtime_test.cpp
        source_test.cpp
    LIBRARIES ks::engine)
