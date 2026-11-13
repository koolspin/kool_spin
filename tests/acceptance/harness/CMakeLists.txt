# Acceptance tests for specs/harness/spec.md: the script reader, the encoder,
# signals and decoding, rendering into a file, playing on miniaudio's null
# device, and the command line. data/ holds small audio fixtures (made with
# ffmpeg) and scripts for the command-line tests.

ks_add_test(ks_harness_acceptance_tests
    SOURCES
        device_test.cpp
        encode_test.cpp
        render_test.cpp
        script_test.cpp
        signals_test.cpp
        tracks_test.cpp
    LIBRARIES ks::harness ks::engine)
target_compile_definitions(ks_harness_acceptance_tests
    PRIVATE KS_HARNESS_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")

# The script reader's fuzz target, run under the fuzz preset only.
ks_add_fuzz_target(ks_harness_script_fuzz
    SOURCES script_fuzz.cpp
    RUNS 200000
    LIBRARIES ks::harness)

# ---- The command line (requirements 2 and 3) -------------------------------
# Patterns are bracket arguments, so they reach grep -E exactly as written.

set(ks_harness $<TARGET_FILE:ks-harness>)
set(data "${CMAKE_CURRENT_SOURCE_DIR}/data")
set(usage [[^Usage: ks-harness \[options\] <script>$]])

ks_acceptance_test(ks-harness.help EXIT 0
    EXPECT ${usage}
    COMMAND ${ks_harness} --help)

ks_acceptance_test(ks-harness.list-devices EXIT 0
    EXPECT [[^ *0: NULL Playback Device \(default\)$]]
    COMMAND ${ks_harness} --list-devices --backend null)

# Usage errors: exit 2, with the usage.
ks_acceptance_test(ks-harness.unknown-option EXIT 2
    EXPECT ${usage}
    COMMAND ${ks_harness} --bogus "${data}/short.harness")
ks_acceptance_test(ks-harness.no-script EXIT 2
    EXPECT ${usage}
    COMMAND ${ks_harness} --backend null)
ks_acceptance_test(ks-harness.unknown-backend EXIT 2
    EXPECT ${usage}
    COMMAND ${ks_harness} --backend wasapi "${data}/short.harness")
ks_acceptance_test(ks-harness.rate-not-a-number EXIT 2
    EXPECT ${usage}
    COMMAND ${ks_harness} --rate fast "${data}/short.harness")
ks_acceptance_test(ks-harness.rate-too-low EXIT 2
    EXPECT ${usage}
    COMMAND ${ks_harness} --rate 7999 "${data}/short.harness")
ks_acceptance_test(ks-harness.rate-too-high EXIT 2
    EXPECT ${usage}
    COMMAND ${ks_harness} --rate 384001 "${data}/short.harness")
ks_acceptance_test(ks-harness.one-channel EXIT 2
    EXPECT ${usage}
    COMMAND ${ks_harness} --channels 1 "${data}/short.harness")
ks_acceptance_test(ks-harness.nine-channels EXIT 2
    EXPECT ${usage}
    COMMAND ${ks_harness} --channels 9 "${data}/short.harness")
ks_acceptance_test(ks-harness.period-too-long EXIT 2
    EXPECT ${usage}
    COMMAND ${ks_harness} --period 4097 "${data}/short.harness")
ks_acceptance_test(ks-harness.zero-in-file-mode EXIT 2
    EXPECT ${usage}
    COMMAND ${ks_harness} --out "${CMAKE_CURRENT_BINARY_DIR}/zero.wav" --rate 0
            "${data}/short.harness")
ks_acceptance_test(ks-harness.backend-in-file-mode EXIT 2
    EXPECT ${usage}
    COMMAND ${ks_harness} --out "${CMAKE_CURRENT_BINARY_DIR}/backend.wav" --backend null
            "${data}/short.harness")
ks_acceptance_test(ks-harness.device-in-file-mode EXIT 2
    EXPECT ${usage}
    COMMAND ${ks_harness} --out "${CMAKE_CURRENT_BINARY_DIR}/device.wav" --device 0
            "${data}/short.harness")

# A device number that isn't listed: the null backend has one device.
ks_acceptance_test(ks-harness.unlisted-device EXIT 2
    COMMAND ${ks_harness} --backend null --device 1 "${data}/short.harness")

# A script error: exit 2, naming the script and the line.
ks_acceptance_test(ks-harness.script-error EXIT 2
    EXPECT [[bad\.harness:3: ]]
    COMMAND ${ks_harness} --check "${data}/bad.harness")

# --check reads the example soak script and prepares its tracks.
ks_acceptance_test(ks-harness.check-soak EXIT 0
    COMMAND ${ks_harness} --check "${PROJECT_SOURCE_DIR}/src/apps/harness/scripts/soak.harness")

# Rendering into a file, and playing on the null device, print the report.
ks_acceptance_test(ks-harness.render EXIT 0
    EXPECT
        [[^Played 1\.000 s$]]
        [[^Commands: 2 sent, 2 read, 0 rejected, 0 delayed$]]
        [[^Device losses: 0$]]
    COMMAND ${ks_harness} --out "${CMAKE_CURRENT_BINARY_DIR}/render.wav" "${data}/short.harness")
ks_acceptance_test(ks-harness.null-device EXIT 0
    EXPECT
        [[NULL Playback Device]]
        [[^Commands: 2 sent, 2 read, 0 rejected, 0 delayed$]]
    COMMAND ${ks_harness} --backend null --rate 48000 --period 512 "${data}/short.harness")
