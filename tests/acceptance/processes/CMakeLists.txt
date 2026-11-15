# Acceptance tests for specs/processes/spec.md: the message types it adds,
# starting children, ks-engine on its own, and kool_spin end to end. Each test
# uses its own runtime directory, so they can run in parallel.

ks_add_test(ks_processes_acceptance_tests
    SOURCES
        engine_process_test.cpp
        messages_test.cpp
        spawn_test.cpp
        supervisor_test.cpp
    LIBRARIES ks::ipc ks::supervisor)

# The socket-protocol tests' helpers (protocol_support.h) and the segments
# tests' temporary directories and descriptor checks (segments_support.h).
target_include_directories(ks_processes_acceptance_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../socket-protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/../segments)

# A helper that reports what it inherited, and stands in for ks-engine.
add_executable(ks_process_probe process_probe.cpp)
target_link_libraries(ks_process_probe PRIVATE ks::ipc ks::warnings)

# The programs the tests run.
target_compile_definitions(ks_processes_acceptance_tests PRIVATE
    KS_KOOL_SPIN_PATH="$<TARGET_FILE:kool_spin>"
    KS_ENGINE_PATH="$<TARGET_FILE:ks-engine>"
    KS_PROBE_PATH="$<TARGET_FILE:ks_process_probe>")
add_dependencies(ks_processes_acceptance_tests kool_spin ks-engine ks_process_probe)
