# Acceptance tests for specs/device-loss/spec.md: Catch2 tests of ks::engine's
# active driver, paced backend, simulated devices, and output controller.
# Tests that wait for something fail, rather than hang, when it hasn't
# happened within the stated time.

ks_add_test(ks_device_loss_acceptance_tests
    SOURCES
        active_driver_test.cpp
        output_controller_test.cpp
        paced_backend_test.cpp
        realtime_test.cpp
    LIBRARIES ks::engine)
