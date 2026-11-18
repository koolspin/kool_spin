# Acceptance tests for specs/ks-play/spec.md: the three new messages, ks-engine
# and kool_spin unloading decks, the client library, and ks-play. Each test
# uses its own runtime directory, so they can run in parallel.

ks_add_test(ks_play_messages_acceptance_tests
    SOURCES
        messages_test.cpp
    LIBRARIES ks::ipc)

ks_add_test(ks_play_unload_acceptance_tests
    SOURCES
        engine_unload_test.cpp
        supervisor_unload_test.cpp
    LIBRARIES ks::decode ks::dsp ks::ipc ks::supervisor)

# The helpers these build on: the track-loading tests' (tracks_support.h) and
# everything they build on.
target_include_directories(ks_play_unload_acceptance_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../track-loading
    ${CMAKE_CURRENT_SOURCE_DIR}/../logs
    ${CMAKE_CURRENT_SOURCE_DIR}/../client-rings
    ${CMAKE_CURRENT_SOURCE_DIR}/../processes
    ${CMAKE_CURRENT_SOURCE_DIR}/../socket-protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/../segments
    ${CMAKE_CURRENT_SOURCE_DIR}/../decode-worker)

# The programs the tests run, and the decode-worker tests' fixtures.
target_compile_definitions(ks_play_unload_acceptance_tests PRIVATE
    KS_KOOL_SPIN_PATH="$<TARGET_FILE:kool_spin>"
    KS_ENGINE_PATH="$<TARGET_FILE:ks-engine>"
    KS_PROBE_PATH="$<TARGET_FILE:ks_process_probe>"
    KS_WORKER_PATH="$<TARGET_FILE:ks-worker>"
    KS_FAKE_WORKER_PATH="$<TARGET_FILE:ks_fake_worker>"
    KS_DECODE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../decode-worker/data")
add_dependencies(ks_play_unload_acceptance_tests
    kool_spin ks-engine ks-worker ks_process_probe ks_fake_worker)
