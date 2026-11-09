# Acceptance tests for specs/ks-tone-options/spec.md. ks-tone runs on miniaudio's
# null backend: a real audio thread at 48 kHz, without hardware. Patterns are
# bracket arguments, so they reach grep -E exactly as written.

set(ks_tone $<TARGET_FILE:ks-tone>)

# ---- Playing (requirements 1-4 and 13) -------------------------------------

ks_acceptance_test(ks-tone-options.defaults EXIT 0
    EXPECT
        [[^Playing on Null: NULL Playback Device$]]
        [[^  440 Hz at -20 dBFS for 2 s$]]
        [[^Done: 9[4-9][0-9]{3} frames, peak -20\.0 dBFS, 440 Hz$]]
    COMMAND ${ks_tone} --backend null)

ks_acceptance_test(ks-tone-options.options EXIT 0
    EXPECT
        [[^  1000 Hz at -12 dBFS for 0\.5 s$]]
        [[^Done: 2[2-7][0-9]{3} frames, peak -12\.0 dBFS, (999|1000|1001) Hz$]]
    COMMAND ${ks_tone} --backend null --frequency 1000 --level -12 --duration 0.5)

ks_acceptance_test(ks-tone-options.lowest EXIT 0
    EXPECT
        [[^  20 Hz at -60 dBFS for 0\.5 s$]]
        [[^Done: [0-9]+ frames, peak -60\.0 dBFS, 20 Hz$]]
    COMMAND ${ks_tone} --backend null --frequency 20 --level -60 --duration 0.5)

ks_acceptance_test(ks-tone-options.highest EXIT 0
    EXPECT
        [[^  20000 Hz at -6 dBFS for 1 s$]]
        [[^Done: [0-9]+ frames, peak -6\.0 dBFS, (199[89][0-9]|200[01][0-9]|20020) Hz$]]
    COMMAND ${ks_tone} --backend null --frequency 20000 --level -6 --duration 1)

ks_acceptance_test(ks-tone-options.decimals EXIT 0
    EXPECT
        [[^  440\.5 Hz at -20 dBFS for 0\.1 s$]]
        [[^Done: [0-9]+ frames, peak -20\.0 dBFS, (440|441) Hz$]]
    COMMAND ${ks_tone} --backend null --frequency 440.5 --duration 0.1)

# ---- Devices (requirements 5, 6, 11, and 14) ---------------------------------

ks_acceptance_test(ks-tone-options.list_devices EXIT 0
    EXPECT
        [[^Output devices on Null:$]]
        [[^  0: NULL Playback Device \(default\)$]]
        [[!^Playing]]
    COMMAND ${ks_tone} --backend null --list-devices)

ks_acceptance_test(ks-tone-options.device EXIT 0
    EXPECT
        [[^Playing on Null: NULL Playback Device$]]
        [[^Done: ]]
    COMMAND ${ks_tone} --backend null --device 0 --duration 0.1)

ks_acceptance_test(ks-tone-options.device_missing EXIT 2
    EXPECT
        [[^ks-tone: no output device 1 on Null$]]
        [[!^Playing]]
    COMMAND ${ks_tone} --backend null --device 1)

# ---- Invalid values (requirements 4 and 8-10) ---------------------------------

# ks_invalid(<name> <expected message> <argument>...): exit code 2, the message
# on a line of its own, and no playback.
function(ks_invalid name message)
    ks_acceptance_test(ks-tone-options.invalid.${name} EXIT 2
        EXPECT "^ks-tone: ${message}$" [[!^Playing]]
        COMMAND ${ks_tone} --backend null ${ARGN})
endfunction()

set(frequency_range "--frequency must be a number from 20 to 20000")
set(level_range "--level must be a number from -60 to -6")
set(duration_range "--duration must be a number from 0.1 to 60")
set(device_number "--device must be a device number shown by --list-devices")

ks_invalid(frequency_too_low "${frequency_range}" --frequency 19.9)
ks_invalid(frequency_too_high "${frequency_range}" --frequency 20001)
ks_invalid(frequency_trailing_text "${frequency_range}" --frequency 440x)
ks_invalid(frequency_missing "--frequency needs a value" --frequency)
ks_invalid(level_too_high "${level_range}" --level -5.9)
ks_invalid(level_too_low "${level_range}" --level -60.1)
ks_invalid(level_nan "${level_range}" --level nan)
ks_invalid(duration_too_short "${duration_range}" --duration 0.09)
ks_invalid(duration_infinite "${duration_range}" --duration inf)
ks_invalid(device_negative "${device_number}" --device -1)
ks_invalid(device_text "${device_number}" --device one)

# ---- Help (requirement 7) -----------------------------------------------------

ks_acceptance_test(ks-tone-options.help EXIT 0
    EXPECT [[--frequency HZ]] [[--level DBFS]] [[--duration SECONDS]] [[--list-devices]] [[--device N]]
    COMMAND ${ks_tone} --help)
