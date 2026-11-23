# ks::ipc: shared memory and sockets for talking between processes (ADR 0003).
# Depends only on ks::core, the standard library, and POSIX, so every process,
# the supervisor included, can use it.

add_library(ks_ipc STATIC
    child_process.cpp
    connection.cpp
    fd.cpp
    fd_passing.cpp
    frame.cpp
    listener.cpp
    log.cpp
    log_format.cpp
    messages.cpp
    recording.cpp
    region.cpp
    runtime_dir.cpp
    segment.cpp
    track_segment.cpp)
add_library(ks::ipc ALIAS ks_ipc)

target_sources(ks_ipc PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/ipc/child_process.h
        include/ks/ipc/command.h
        include/ks/ipc/connection.h
        include/ks/ipc/crc32.h
        include/ks/ipc/fd.h
        include/ks/ipc/fd_passing.h
        include/ks/ipc/frame.h
        include/ks/ipc/layout.h
        include/ks/ipc/listener.h
        include/ks/ipc/log.h
        include/ks/ipc/messages.h
        include/ks/ipc/recording.h
        include/ks/ipc/region.h
        include/ks/ipc/runtime_dir.h
        include/ks/ipc/segment.h
        include/ks/ipc/seqlock.h
        include/ks/ipc/spsc_ring.h
        include/ks/ipc/state.h
        include/ks/ipc/track_segment.h)

target_link_libraries(ks_ipc
    PUBLIC ks::core
    PRIVATE ks::warnings)
