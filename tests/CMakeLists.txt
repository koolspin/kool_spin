find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# ks_add_test(<name> SOURCES <file>... [LIBRARIES <target>...])
#
# Builds a Catch2 test executable and registers its test cases with CTest.
function(ks_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;LIBRARIES")
    add_executable(${name} ${arg_SOURCES})
    target_link_libraries(${name} PRIVATE ${arg_LIBRARIES} ks::warnings Catch2::Catch2WithMain)
    # Discover test cases when CTest runs, not at build time, so a sanitizer
    # report shows up as a failing test rather than a broken build.
    catch_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

ks_add_test(ks_core_tests
    SOURCES core/realtime_test.cpp
    LIBRARIES ks::core)

ks_add_test(ks_client_tests
    SOURCES
        client/command_test.cpp
        client/decode_queue_test.cpp
        client/session_test.cpp
        client/snapshot_test.cpp
    LIBRARIES ks::client)
# ks::client's private headers, such as decode_queue.h.
target_include_directories(ks_client_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/client)

ks_add_test(ks_play_app_tests
    SOURCES
        play_app/numbers_test.cpp
        play_app/options_test.cpp
        play_app/run_test.cpp
        play_app/status_format_test.cpp
    LIBRARIES ks::play_app)
# ks_play_app's private headers, such as numbers.h.
target_include_directories(ks_play_app_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/apps/play)

ks_add_test(ks_dsp_tests
    SOURCES dsp/sine_oscillator_test.cpp
            dsp/frequency_meter_test.cpp
    LIBRARIES ks::dsp)

ks_add_test(ks_engine_tests
    SOURCES
        engine/command_test.cpp
        engine/deck_test.cpp
        engine/device_loss_test.cpp
        engine/engine_test.cpp
        engine/mixer_test.cpp
        engine/render_core_test.cpp
        engine/resume_test.cpp
        engine/safety_test.cpp
    LIBRARIES ks::engine)

ks_add_test(ks_harness_tests
    SOURCES
        harness/device_mode_test.cpp
        harness/encode_test.cpp
        harness/file_mode_test.cpp
        harness/options_test.cpp
        harness/player_test.cpp
        harness/report_test.cpp
        harness/run_test.cpp
        harness/script_lexer_test.cpp
        harness/script_test.cpp
        harness/signals_test.cpp
        harness/tracks_test.cpp
    LIBRARIES ks::harness)
# The harness's private headers, such as script_lexer.h.
target_include_directories(ks_harness_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/apps/harness)

ks_add_test(ks_ipc_tests
    SOURCES
        ipc/log_format_test.cpp
        ipc/log_test.cpp
        ipc/region_test.cpp
        ipc/seqlock_test.cpp
        ipc/spsc_ring_test.cpp
    LIBRARIES ks::ipc)

ks_add_test(ks_engine_app_tests
    SOURCES engine_app/segment_track_test.cpp
    LIBRARIES ks::engine_app)

ks_add_test(ks_supervisor_tests
    SOURCES supervisor/log_dir_test.cpp
            supervisor/log_file_test.cpp
            supervisor/run_logs_test.cpp
    LIBRARIES ks::supervisor)
# run_logs_test.cpp runs the real engine to a starting failure.
target_compile_definitions(ks_supervisor_tests PRIVATE
    KS_ENGINE_PATH="$<TARGET_FILE:ks-engine>"
    KS_WORKER_PATH="$<TARGET_FILE:ks-worker>")
add_dependencies(ks_supervisor_tests ks-engine ks-worker)

ks_add_test(ks_tone_tests
    SOURCES
        tone/options_test.cpp
        tone/tone_generator_test.cpp
        tone/tone_player_test.cpp
    LIBRARIES ks::tone)

# ks-tone itself, end to end, without audio hardware.
add_test(NAME ks-tone.plays_on_null_backend COMMAND ks-tone --backend null)
set_tests_properties(ks-tone.plays_on_null_backend PROPERTIES
    PASS_REGULAR_EXPRESSION "Playing on Null:.*Done: [1-9][0-9]* frames, peak -20.0 dBFS")
add_test(NAME ks-tone.rejects_unknown_arguments COMMAND ks-tone --bogus)
set_tests_properties(ks-tone.rejects_unknown_arguments PROPERTIES
    PASS_REGULAR_EXPRESSION "unexpected argument '--bogus'")

# Acceptance tests, one directory per feature, labeled 'acceptance'.
add_subdirectory(acceptance)

# Guardrail: prove the protected-path hook and CI check work (docs/process.md).
add_test(NAME guardrail.protected_paths
    COMMAND bash "${PROJECT_SOURCE_DIR}/tests/guardrails/protected_paths_test.sh")

# Guardrail: prove RealtimeSanitizer reports real-time violations. The canary
# is built in every preset, so it is always compiled and linted, but it is
# registered as a test only in the rtsan preset, where each mode must produce
# a RealtimeSanitizer report. Other presets have nothing that could detect it.
add_executable(ks_rtsan_canary guardrails/rtsan_canary.cpp)
target_link_libraries(ks_rtsan_canary PRIVATE ks::core ks::warnings)

if(KS_SANITIZER STREQUAL "rtsan")
    add_test(NAME guardrail.rtsan_reports_allocation COMMAND ks_rtsan_canary allocate)
    add_test(NAME guardrail.rtsan_reports_locking COMMAND ks_rtsan_canary lock)
    # Pass only on the expected report; the exit code is ignored.
    set_tests_properties(guardrail.rtsan_reports_allocation PROPERTIES
        PASS_REGULAR_EXPRESSION "Intercepted call to real-time unsafe function `malloc` in real-time context")
    set_tests_properties(guardrail.rtsan_reports_locking PROPERTIES
        PASS_REGULAR_EXPRESSION "Intercepted call to real-time unsafe function `pthread_mutex_lock` in real-time context")
endif()
