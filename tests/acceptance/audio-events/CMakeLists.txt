# Acceptance tests for specs/audio-events/spec.md: the events the engine's
# audio thread records, EventLines turning them into summarized log lines,
# and ks-engine logging them and taking its signals on the control thread,
# with the test as its supervisor. Each process test uses its own runtime
# directory, so they can run in parallel.

# The engine and EventLines, in the test's process. They use the commands
# tests' clients and tracks.
ks_add_test(ks_audio_events_acceptance_tests
    SOURCES
        engine_events_test.cpp
        event_lines_test.cpp
    LIBRARIES ks::engine)

# ks-engine, as a process.
ks_add_test(ks_engine_events_acceptance_tests
    SOURCES engine_process_test.cpp
    LIBRARIES ks::ipc ks::supervisor)

# The logs tests' helpers (logs_support.h), and through them the client-rings,
# processes, socket-protocol, and segments tests' helpers.
target_include_directories(ks_engine_events_acceptance_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../logs
    ${CMAKE_CURRENT_SOURCE_DIR}/../client-rings
    ${CMAKE_CURRENT_SOURCE_DIR}/../processes
    ${CMAKE_CURRENT_SOURCE_DIR}/../socket-protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/../segments)

# The programs the tests run. processes_support.h names them all.
target_compile_definitions(ks_engine_events_acceptance_tests PRIVATE
    KS_KOOL_SPIN_PATH="$<TARGET_FILE:kool_spin>"
    KS_ENGINE_PATH="$<TARGET_FILE:ks-engine>"
    KS_PROBE_PATH="$<TARGET_FILE:ks_process_probe>")
add_dependencies(ks_engine_events_acceptance_tests kool_spin ks-engine ks_process_probe)
