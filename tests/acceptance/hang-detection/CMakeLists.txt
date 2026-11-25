# Acceptance tests for specs/hang-detection/spec.md: kool_spin killing an
# engine whose heartbeat stops, and reporting (and, once no deck plays,
# killing) one whose control thread stops answering. They build on the
# engine-restart tests' helpers. Each test uses its own runtime directory, so
# they can run in parallel.

ks_add_test(ks_hang_supervisor_acceptance_tests
    SOURCES
        hang_test.cpp
    LIBRARIES ks::decode ks::dsp ks::ipc ks::supervisor)

target_include_directories(ks_hang_supervisor_acceptance_tests PRIVATE
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
target_compile_definitions(ks_hang_supervisor_acceptance_tests PRIVATE
    KS_KOOL_SPIN_PATH="$<TARGET_FILE:kool_spin>"
    KS_ENGINE_PATH="$<TARGET_FILE:ks-engine>"
    KS_PROBE_PATH="$<TARGET_FILE:ks_process_probe>"
    KS_WORKER_PATH="$<TARGET_FILE:ks-worker>"
    KS_FAKE_WORKER_PATH="$<TARGET_FILE:ks_fake_worker>"
    KS_PLAY_PATH="$<TARGET_FILE:ks-play>"
    KS_DECODE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../decode-worker/data")
add_dependencies(ks_hang_supervisor_acceptance_tests
    kool_spin ks-engine ks-worker ks-play ks_process_probe ks_fake_worker)
