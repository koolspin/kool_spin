# ks-worker: the decode worker, started by kool_spin for each track
# (specs/decode-worker/spec.md).

# The library holds everything except argument handling, so tests can use it.
add_library(ks_worker_app STATIC
    options.cpp
    run.cpp)
add_library(ks::worker_app ALIAS ks_worker_app)

target_sources(ks_worker_app PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/worker_app/options.h
        include/ks/worker_app/run.h)

target_link_libraries(ks_worker_app
    PUBLIC ks::decode ks::ipc
    PRIVATE ks::warnings)

add_executable(ks-worker main.cpp)
target_link_libraries(ks-worker PRIVATE ks::worker_app ks::warnings)

# Next to kool_spin and ks-engine (specs/decode-worker/spec.md requirement
# 15).
set_target_properties(ks-worker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
