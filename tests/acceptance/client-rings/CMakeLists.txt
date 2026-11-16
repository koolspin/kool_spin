# Acceptance tests for specs/client-rings/spec.md: the ring messages and
# segment, ks-engine attaching rings with the test as its supervisor, and
# kool_spin giving clients rings end to end. Each test uses its own runtime
# directory, so they can run in parallel.

ks_add_test(ks_client_rings_acceptance_tests
    SOURCES
        engine_rings_test.cpp
        messages_test.cpp
        supervisor_rings_test.cpp
    LIBRARIES ks::ipc ks::supervisor)

# The processes tests' helpers (processes_support.h), and through them the
# socket-protocol and segments tests' helpers.
target_include_directories(ks_client_rings_acceptance_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../processes
    ${CMAKE_CURRENT_SOURCE_DIR}/../socket-protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/../segments)

# The programs the tests run. processes_support.h names the probe too.
target_compile_definitions(ks_client_rings_acceptance_tests PRIVATE
    KS_KOOL_SPIN_PATH="$<TARGET_FILE:kool_spin>"
    KS_ENGINE_PATH="$<TARGET_FILE:ks-engine>"
    KS_PROBE_PATH="$<TARGET_FILE:ks_process_probe>")
add_dependencies(ks_client_rings_acceptance_tests kool_spin ks-engine ks_process_probe)
