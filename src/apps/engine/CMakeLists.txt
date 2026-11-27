# ks-engine: the engine process, started by kool_spin (specs/processes/spec.md).

# The library holds everything except argument handling, so tests can use it.
add_library(ks_engine_app STATIC
    health.cpp
    options.cpp
    run.cpp
    segment_track.cpp)
add_library(ks::engine_app ALIAS ks_engine_app)

target_sources(ks_engine_app PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/engine_app/health.h
        include/ks/engine_app/options.h
        include/ks/engine_app/run.h
        include/ks/engine_app/segment_track.h)

target_link_libraries(ks_engine_app
    PUBLIC ks::engine ks::ipc
    PRIVATE ks::warnings)

add_executable(ks-engine main.cpp)
target_link_libraries(ks-engine PRIVATE ks::engine_app ks::warnings)

# Next to kool_spin, which looks for it there (specs/processes/spec.md
# requirement 3).
set_target_properties(ks-engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
