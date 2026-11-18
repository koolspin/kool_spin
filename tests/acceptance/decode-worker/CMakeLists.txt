# Acceptance tests for specs/decode-worker/spec.md: the track segment, the
# probe and decode messages, decoding with FFmpeg, and ks-worker as a process
# with the test as its supervisor. data/ holds small audio fixtures, made by
# data/make.sh with the ffmpeg command-line tool. Each test that writes files
# or segments uses its own temporary directory, so they can run in parallel.

# The track segment, the messages, and decoding, in the test's process.
ks_add_test(ks_decode_worker_acceptance_tests
    SOURCES
        decode_track_test.cpp
        decoder_test.cpp
        messages_test.cpp
        track_segment_test.cpp
    LIBRARIES ks::decode ks::dsp ks::engine ks::ipc)

# ks-worker, as a process.
ks_add_test(ks_worker_acceptance_tests
    SOURCES worker_test.cpp
    LIBRARIES ks::decode ks::dsp ks::ipc ks::supervisor)

foreach(target ks_decode_worker_acceptance_tests ks_worker_acceptance_tests)
    # The segments tests' temporary directories (segments_support.h), and,
    # for the process tests, the processes tests' helpers
    # (processes_support.h) and the socket-protocol tests' (protocol_support.h).
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../segments
        ${CMAKE_CURRENT_SOURCE_DIR}/../processes
        ${CMAKE_CURRENT_SOURCE_DIR}/../socket-protocol)
    target_compile_definitions(${target} PRIVATE
        KS_DECODE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
endforeach()

# The programs the process tests run. processes_support.h names the
# processes tests' programs too.
target_compile_definitions(ks_worker_acceptance_tests PRIVATE
    KS_WORKER_PATH="$<TARGET_FILE:ks-worker>"
    KS_KOOL_SPIN_PATH="$<TARGET_FILE:kool_spin>"
    KS_ENGINE_PATH="$<TARGET_FILE:ks-engine>"
    KS_PROBE_PATH="$<TARGET_FILE:ks_process_probe>")
add_dependencies(ks_worker_acceptance_tests ks-worker kool_spin ks-engine ks_process_probe)

# The track segment header's fuzz target, run under the fuzz preset only.
ks_add_fuzz_target(ks_track_segment_fuzz
    SOURCES track_segment_fuzz.cpp
    RUNS 1000000
    LIBRARIES ks::ipc)
