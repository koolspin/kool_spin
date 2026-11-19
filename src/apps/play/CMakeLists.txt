# ks-play: the command-line client for kool_spin (specs/ks-play/spec.md).

# The library holds everything except argument passing, so tests can use it.
add_library(ks_play_app STATIC
    numbers.cpp
    options.cpp
    run.cpp
    status_format.cpp)
add_library(ks::play_app ALIAS ks_play_app)

target_sources(ks_play_app PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/play_app/options.h
        include/ks/play_app/run.h)

target_link_libraries(ks_play_app
    PUBLIC ks::client
    PRIVATE ks::warnings)

add_executable(ks-play main.cpp)
target_link_libraries(ks-play PRIVATE ks::play_app ks::warnings)

# Next to kool_spin (specs/ks-play/spec.md requirement 30).
set_target_properties(ks-play PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
