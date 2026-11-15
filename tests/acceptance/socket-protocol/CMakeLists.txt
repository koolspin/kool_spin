# Acceptance tests for specs/socket-protocol/spec.md: Catch2 tests of ks::ipc's
# frames, connections, the supervisor's socket, and message types. Each test
# that needs a runtime directory uses its own temporary one, so they can run in
# parallel.

ks_add_test(ks_socket_protocol_acceptance_tests
    SOURCES
        connection_test.cpp
        frame_test.cpp
        listener_test.cpp
        messages_test.cpp
    LIBRARIES ks::ipc)

# The segments tests' forked children, temporary directories, and descriptor
# counting (segments_support.h).
target_include_directories(ks_socket_protocol_acceptance_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../segments)

# The frame reader and message decoder fuzz target, run under the fuzz preset
# only.
ks_add_fuzz_target(ks_socket_protocol_fuzz
    SOURCES protocol_fuzz.cpp
    RUNS 1000000
    LIBRARIES ks::ipc)
