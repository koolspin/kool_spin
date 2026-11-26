# Acceptance tests for specs/session-journal/spec.md, step 9b: the journal's
# format and its fuzz target.

ks_add_test(ks_journal_format_acceptance_tests
    SOURCES journal_format_test.cpp
    LIBRARIES ks::engine ks::supervisor)

# The parser's fuzz target, run under the fuzz preset only.
ks_add_fuzz_target(ks_journal_fuzz
    SOURCES journal_fuzz.cpp
    RUNS 200000
    LIBRARIES ks::engine ks::supervisor)
