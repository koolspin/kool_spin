# Acceptance tests for specs/state-folder/spec.md: the state folder, the locks
# in it, and finding an orphan through it. They build on the supervisor-crash
# tests' helpers. Each test uses runtime, state, and log folders of its own, so
# they can run in parallel.

ks_add_test(ks_state_dir_acceptance_tests
    SOURCES state_dir_test.cpp
    LIBRARIES ks::ipc)
target_include_directories(ks_state_dir_acceptance_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../segments)

ks_add_test(ks_state_folder_acceptance_tests
    SOURCES
        state_locks_test.cpp
        state_orphan_test.cpp
    LIBRARIES ks::client ks::decode ks::dsp ks::ipc ks::supervisor)

target_include_directories(ks_state_folder_acceptance_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../supervisor-crash
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine-restart
    ${CMAKE_CURRENT_SOURCE_DIR}/../track-loading
    ${CMAKE_CURRENT_SOURCE_DIR}/../logs
    ${CMAKE_CURRENT_SOURCE_DIR}/../client-rings
    ${CMAKE_CURRENT_SOURCE_DIR}/../processes
    ${CMAKE_CURRENT_SOURCE_DIR}/../socket-protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/../segments
    ${CMAKE_CURRENT_SOURCE_DIR}/../decode-worker)

# The programs the tests run, and the decode-worker tests' fixtures.
target_compile_definitions(ks_state_folder_acceptance_tests PRIVATE
    KS_LOCK_HOLDER_PATH="$<TARGET_FILE:ks_lock_holder>"
    KS_ON_TERMINAL_PATH="$<TARGET_FILE:ks_on_terminal>"
    KS_KOOL_SPIN_PATH="$<TARGET_FILE:kool_spin>"
    KS_ENGINE_PATH="$<TARGET_FILE:ks-engine>"
    KS_PROBE_PATH="$<TARGET_FILE:ks_process_probe>"
    KS_WORKER_PATH="$<TARGET_FILE:ks-worker>"
    KS_FAKE_WORKER_PATH="$<TARGET_FILE:ks_fake_worker>"
    KS_PLAY_PATH="$<TARGET_FILE:ks-play>"
    KS_DECODE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../decode-worker/data")
add_dependencies(ks_state_folder_acceptance_tests
    kool_spin ks-engine ks-worker ks-play ks_process_probe ks_fake_worker
    ks_lock_holder ks_on_terminal)
