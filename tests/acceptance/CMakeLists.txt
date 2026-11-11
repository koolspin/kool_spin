# Acceptance tests: one directory per feature, written and approved before a
# build loop starts (docs/process.md). Everything here is protected.
#
# Every test here has the CTest label 'acceptance'. While a feature is being
# built, its acceptance tests are expected to fail, so build passes check for
# regressions with `ctest -LE acceptance`. A finished feature, and CI, must pass
# them all.

set_property(DIRECTORY PROPERTY LABELS acceptance)

# ks_acceptance_test(<name> EXIT <code> [EXPECT <pattern>...] COMMAND <command> [<arg>...])
#
# Runs <command> and checks its exit code and output with expect.sh. Each
# pattern is an extended regular expression that must match a line of output;
# a pattern starting with '!' must match none.
function(ks_acceptance_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "EXIT" "EXPECT;COMMAND")
    add_test(NAME ${name}
        COMMAND bash "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/expect.sh" ${arg_EXIT} ${arg_EXPECT}
                -- ${arg_COMMAND})
endfunction()

# ks_add_fuzz_target(), for features whose input is fuzzed.
include(KsFuzz)

# Each feature's directory registers its own tests.
file(GLOB entries LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(entry IN LISTS entries)
    if(IS_DIRECTORY "${entry}" AND EXISTS "${entry}/CMakeLists.txt")
        add_subdirectory("${entry}")
    endif()
endforeach()
