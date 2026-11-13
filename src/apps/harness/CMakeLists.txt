# ks-harness: drives the engine from a script, for listening and soak tests
# (specs/harness/spec.md). Not installed or shipped.

# The library holds everything except argument handling, so tests can use it.
add_library(ks_harness STATIC
    encode.cpp
    options.cpp
    player.cpp
    run.cpp
    script.cpp
    script_lexer.cpp
    tracks.cpp)
add_library(ks::harness ALIAS ks_harness)

target_sources(ks_harness PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/harness/encode.h
        include/ks/harness/options.h
        include/ks/harness/run.h
        include/ks/harness/script.h
        include/ks/harness/tracks.h)

# The codecs are private: only the harness decodes files (requirement 31).
target_link_libraries(ks_harness
    PUBLIC ks::engine ks::ipc
    PRIVATE ks::miniaudio_codecs ks::warnings)

add_executable(ks-harness main.cpp)
target_link_libraries(ks-harness PRIVATE ks::harness ks::warnings)
