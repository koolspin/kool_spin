# Acceptance tests for specs/health-checks/spec.md, step 10a: the health
# block's layout, the engine's peak load, the health pass, reading the power
# profile and source, and ks-engine's options, in the test's process; and
# kool_spin's engine checking its health, with ks-play printing it. Each
# process test uses runtime and log folders of its own, so they can run in
# parallel.

ks_add_test(ks_health_acceptance_tests
    SOURCES health_test.cpp
    LIBRARIES ks::engine ks::engine_app ks::ipc)

# Sets resource limits, then executes kool_spin, for the process tests.
add_executable(ks_rlimit_exec rlimit_exec.cpp)
target_link_libraries(ks_rlimit_exec PRIVATE ks::warnings)

ks_add_test(ks_health_process_acceptance_tests
    SOURCES health_process_test.cpp
    LIBRARIES ks::client ks::decode ks::dsp ks::engine ks::ipc ks::supervisor)

# The helpers these build on: the engine-restart and ks-play tests', and
# everything they build on.
target_include_directories(ks_health_process_acceptance_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine-restart
    ${CMAKE_CURRENT_SOURCE_DIR}/../ks-play
    ${CMAKE_CURRENT_SOURCE_DIR}/../track-loading
    ${CMAKE_CURRENT_SOURCE_DIR}/../logs
    ${CMAKE_CURRENT_SOURCE_DIR}/../client-rings
    ${CMAKE_CURRENT_SOURCE_DIR}/../processes
    ${CMAKE_CURRENT_SOURCE_DIR}/../socket-protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/../segments
    ${CMAKE_CURRENT_SOURCE_DIR}/../decode-worker)

# The programs the tests run, and the decode-worker tests' fixtures.
target_compile_definitions(ks_health_process_acceptance_tests PRIVATE
    KS_KOOL_SPIN_PATH="$<TARGET_FILE:kool_spin>"
    KS_ENGINE_PATH="$<TARGET_FILE:ks-engine>"
    KS_PROBE_PATH="$<TARGET_FILE:ks_process_probe>"
    KS_WORKER_PATH="$<TARGET_FILE:ks-worker>"
    KS_FAKE_WORKER_PATH="$<TARGET_FILE:ks_fake_worker>"
    KS_PLAY_PATH="$<TARGET_FILE:ks-play>"
    KS_RLIMIT_EXEC_PATH="$<TARGET_FILE:ks_rlimit_exec>"
    KS_DECODE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../decode-worker/data")
add_dependencies(ks_health_process_acceptance_tests
    kool_spin ks-engine ks-worker ks-play ks_process_probe ks_fake_worker ks_rlimit_exec)
