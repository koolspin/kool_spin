# kool_spin: the supervisor (specs/processes/spec.md). It depends only on
# ks::ipc, the standard library, and POSIX (ADR 0002 §1).

# The library holds everything except argument handling, so tests can use it.
add_library(ks_supervisor STATIC
    log_file.cpp
    options.cpp
    run.cpp
    spawn.cpp)
add_library(ks::supervisor ALIAS ks_supervisor)

target_sources(ks_supervisor PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/supervisor/log_file.h
        include/ks/supervisor/options.h
        include/ks/supervisor/run.h
        include/ks/supervisor/spawn.h)

target_link_libraries(ks_supervisor
    PUBLIC ks::ipc
    PRIVATE ks::warnings)

add_executable(kool_spin main.cpp)
target_link_libraries(kool_spin PRIVATE ks::supervisor ks::warnings)

# kool_spin finds ks-engine next to itself (requirement 3).
set_target_properties(kool_spin PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
