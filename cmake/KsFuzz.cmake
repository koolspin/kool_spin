# Fuzz targets (ADR 0003 §8). Under the fuzz preset (KS_SANITIZER=fuzz), the
# whole build has libFuzzer's coverage instrumentation, ASan, and UBSan; a fuzz
# target links libFuzzer's driver, and CTest runs it for a fixed number of
# inputs from a fixed seed, so a run is repeatable. Other presets build
# nothing here: keep a target's checks in a header that an ordinary test also
# includes, so they're compiled and linted everywhere.

# ks_add_fuzz_target(<name> SOURCES <file>... RUNS <count> [LIBRARIES <target>...])
#
# Builds the libFuzzer target <name> from SOURCES, which define
# LLVMFuzzerTestOneInput, and registers a test of the same name that runs it
# for <count> inputs. A crash, a sanitizer report, or a failed check fails it.
function(ks_add_fuzz_target name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "RUNS" "SOURCES;LIBRARIES")
    if(NOT arg_RUNS)
        message(FATAL_ERROR "ks_add_fuzz_target(${name}): RUNS is required")
    endif()
    if(NOT KS_SANITIZER STREQUAL "fuzz")
        return()
    endif()
    add_executable(${name} ${arg_SOURCES})
    target_link_libraries(${name} PRIVATE ${arg_LIBRARIES} ks::warnings)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    add_test(NAME ${name} COMMAND ${name} -runs=${arg_RUNS} -seed=1)
endfunction()
