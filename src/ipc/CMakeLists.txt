# ks::ipc: shared-memory primitives for talking between processes (ADR 0003).
# Depends only on ks::core, the standard library, and POSIX, so every process,
# the supervisor included, can use it.

add_library(ks_ipc STATIC
    region.cpp)
add_library(ks::ipc ALIAS ks_ipc)

target_sources(ks_ipc PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/ipc/command.h
        include/ks/ipc/crc32.h
        include/ks/ipc/layout.h
        include/ks/ipc/region.h
        include/ks/ipc/seqlock.h
        include/ks/ipc/spsc_ring.h
        include/ks/ipc/state.h)

target_link_libraries(ks_ipc
    PUBLIC ks::core
    PRIVATE ks::warnings)
