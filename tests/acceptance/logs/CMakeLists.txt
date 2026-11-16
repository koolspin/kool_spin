# Acceptance tests for specs/logs/spec.md: the log segment, writer, reader,
# formats, and files; ks-engine logging through its segment with the test as
# its supervisor; and kool_spin's session folders end to end. Each test uses
# its own runtime directory and log folder, so they can run in parallel.

ks_add_test(ks_logs_acceptance_tests
    SOURCES
        engine_logs_test.cpp
        log_test.cpp
        supervisor_logs_test.cpp
    LIBRARIES ks::ipc ks::supervisor)

# The client-rings tests' helpers (rings_support.h), and through them the
# processes, socket-protocol, and segments tests' helpers.
target_include_directories(ks_logs_acceptance_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../client-rings
    ${CMAKE_CURRENT_SOURCE_DIR}/../processes
    ${CMAKE_CURRENT_SOURCE_DIR}/../socket-protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/../segments)

# The programs the tests run. processes_support.h names the probe too.
target_compile_definitions(ks_logs_acceptance_tests PRIVATE
    KS_KOOL_SPIN_PATH="$<TARGET_FILE:kool_spin>"
    KS_ENGINE_PATH="$<TARGET_FILE:ks-engine>"
    KS_PROBE_PATH="$<TARGET_FILE:ks_process_probe>")
add_dependencies(ks_logs_acceptance_tests kool_spin ks-engine ks_process_probe)

# The record formatter's fuzz target, run under the fuzz preset only. Its
# checks are in record_check.h, which "Log lines: format" also runs.
ks_add_fuzz_target(ks_log_record_fuzz
    SOURCES record_fuzz.cpp
    RUNS 1000000
    LIBRARIES ks::ipc ks::supervisor)
