# ks::client: a client's session with kool_spin (specs/ks-play/spec.md). Depends
# only on ks::core, ks::ipc, the standard library, and POSIX, so the UI can use
# it too.

add_library(ks_client STATIC
    command.cpp
    session.cpp
    snapshot.cpp)
add_library(ks::client ALIAS ks_client)

target_sources(ks_client PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/client/command.h
        include/ks/client/session.h
        include/ks/client/snapshot.h)

target_link_libraries(ks_client
    PUBLIC ks::core ks::ipc
    PRIVATE ks::warnings)
