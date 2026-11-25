# Acceptance tests for specs/supervisor-crash/spec.md: lock files, finding and
# stopping an orphaned engine, playing on after the supervisor dies, launching
# again, and what ks-play says. They build on the engine-restart tests'
# helpers. Each test uses its own runtime directory, so they can run in
# parallel.

# A process that holds the engine lock, as an orphan would.
add_executable(ks_lock_holder lock_holder.cpp)
target_link_libraries(ks_lock_holder PRIVATE ks::ipc ks::warnings)

# Runs a program on a terminal of its own.
add_executable(ks_on_terminal on_terminal.cpp)
target_link_libraries(ks_on_terminal PRIVATE ks::warnings)

ks_add_test(ks_crash_orphan_acceptance_tests
    SOURCES
        lock_test.cpp
        orphan_test.cpp
    LIBRARIES ks::ipc ks::supervisor)

ks_add_test(ks_crash_supervisor_acceptance_tests
    SOURCES
        crash_test.cpp
        play_hint_test.cpp
        relaunch_test.cpp
    LIBRARIES ks::client ks::decode ks::dsp ks::ipc ks::supervisor)

foreach(target ks_crash_orphan_acceptance_tests ks_crash_supervisor_acceptance_tests)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../engine-restart
        ${CMAKE_CURRENT_SOURCE_DIR}/../track-loading
        ${CMAKE_CURRENT_SOURCE_DIR}/../logs
        ${CMAKE_CURRENT_SOURCE_DIR}/../client-rings
        ${CMAKE_CURRENT_SOURCE_DIR}/../processes
        ${CMAKE_CURRENT_SOURCE_DIR}/../socket-protocol
        ${CMAKE_CURRENT_SOURCE_DIR}/../segments
        ${CMAKE_CURRENT_SOURCE_DIR}/../decode-worker)

    # The programs the tests run, and the decode-worker tests' fixtures.
    target_compile_definitions(${target} PRIVATE
        KS_LOCK_HOLDER_PATH="$<TARGET_FILE:ks_lock_holder>"
        KS_ON_TERMINAL_PATH="$<TARGET_FILE:ks_on_terminal>"
        KS_KOOL_SPIN_PATH="$<TARGET_FILE:kool_spin>"
        KS_ENGINE_PATH="$<TARGET_FILE:ks-engine>"
        KS_PROBE_PATH="$<TARGET_FILE:ks_process_probe>"
        KS_WORKER_PATH="$<TARGET_FILE:ks-worker>"
        KS_FAKE_WORKER_PATH="$<TARGET_FILE:ks_fake_worker>"
        KS_PLAY_PATH="$<TARGET_FILE:ks-play>"
        KS_DECODE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../decode-worker/data")
    add_dependencies(${target}
        kool_spin ks-engine ks-worker ks-play ks_process_probe ks_fake_worker
        ks_lock_holder ks_on_terminal)
endforeach()
