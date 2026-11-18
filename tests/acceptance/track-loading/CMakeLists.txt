# Acceptance tests for specs/track-loading/spec.md: the track-loading
# messages, ks-engine loading and releasing track segments with the test as
# its supervisor and worker, and kool_spin loading files end to end, with
# ks-worker or ks_fake_worker. Each test uses its own runtime directory, so
# they can run in parallel.

ks_add_test(ks_track_loading_acceptance_tests
    SOURCES
        engine_tracks_test.cpp
        messages_test.cpp
        supervisor_tracks_test.cpp
    LIBRARIES ks::decode ks::dsp ks::ipc ks::supervisor)

# The helpers this builds on: the logs tests' (logs_support.h), the
# client-rings tests' (rings_support.h), the processes tests'
# (processes_support.h), the socket-protocol and segments tests', and the
# decode-worker tests' fixtures and WAV writer (decode_support.h).
target_include_directories(ks_track_loading_acceptance_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../logs
    ${CMAKE_CURRENT_SOURCE_DIR}/../client-rings
    ${CMAKE_CURRENT_SOURCE_DIR}/../processes
    ${CMAKE_CURRENT_SOURCE_DIR}/../socket-protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/../segments
    ${CMAKE_CURRENT_SOURCE_DIR}/../decode-worker)

# A stand-in for ks-worker whose behavior comes from the file it probes.
add_executable(ks_fake_worker fake_worker.cpp)
target_link_libraries(ks_fake_worker PRIVATE ks::ipc ks::warnings)

# The programs the tests run, and the decode-worker tests' fixtures.
target_compile_definitions(ks_track_loading_acceptance_tests PRIVATE
    KS_KOOL_SPIN_PATH="$<TARGET_FILE:kool_spin>"
    KS_ENGINE_PATH="$<TARGET_FILE:ks-engine>"
    KS_PROBE_PATH="$<TARGET_FILE:ks_process_probe>"
    KS_WORKER_PATH="$<TARGET_FILE:ks-worker>"
    KS_FAKE_WORKER_PATH="$<TARGET_FILE:ks_fake_worker>"
    KS_DECODE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../decode-worker/data")
add_dependencies(ks_track_loading_acceptance_tests
    kool_spin ks-engine ks-worker ks_process_probe ks_fake_worker)
