# Acceptance tests for specs/ks-play/spec.md: the three new messages, ks-engine
# and kool_spin unloading decks, the client library, and ks-play. Each test
# uses its own runtime directory, so they can run in parallel.

ks_add_test(ks_play_messages_acceptance_tests
    SOURCES
        messages_test.cpp
    LIBRARIES ks::ipc)
