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
