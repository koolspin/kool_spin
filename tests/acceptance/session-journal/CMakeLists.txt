# Acceptance tests for specs/session-journal/spec.md: the journal's format and
# its fuzz target, the supervisor writing it (step 9b), and restoring the
# session from it at launch (step 9c). Each test that runs the programs uses
# runtime, state, and log folders of its own, so they can run in parallel.
# They build on the state-folder and engine-restart tests' helpers.

ks_add_test(ks_journal_format_acceptance_tests
    SOURCES journal_format_test.cpp
    LIBRARIES ks::engine ks::supervisor)

# The parser's fuzz target, run under the fuzz preset only.
ks_add_fuzz_target(ks_journal_fuzz
    SOURCES journal_fuzz.cpp
    RUNS 200000
    LIBRARIES ks::engine ks::supervisor)

ks_add_test(ks_journal_write_acceptance_tests
    SOURCES journal_write_test.cpp
    LIBRARIES ks::client ks::decode ks::dsp ks::engine ks::ipc ks::supervisor)

ks_add_test(ks_journal_restore_acceptance_tests
    SOURCES journal_restore_test.cpp
    LIBRARIES ks::client ks::decode ks::dsp ks::engine ks::ipc ks::supervisor)

foreach(target ks_journal_write_acceptance_tests ks_journal_restore_acceptance_tests)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../state-folder
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
