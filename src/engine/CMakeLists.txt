# ks::engine: the audio engine (ADR 0003). It renders offline, for tests, or
# through a device with miniaudio. No Qt.

add_library(ks_engine STATIC
    block.cpp
    interleave.cpp
    miniaudio_backend.cpp
    offline.cpp
    output_controller.cpp
    paced_backend.cpp
    render_core.cpp
    safety.cpp)
add_library(ks::engine ALIAS ks_engine)

target_sources(ks_engine PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/engine/audio_backend.h
        include/ks/engine/block.h
        include/ks/engine/interleave.h
        include/ks/engine/miniaudio_backend.h
        include/ks/engine/offline.h
        include/ks/engine/output_controller.h
        include/ks/engine/paced_backend.h
        include/ks/engine/render_core.h
        include/ks/engine/safety.h)

target_link_libraries(ks_engine
    PUBLIC ks::core
    PRIVATE ks::miniaudio ks::warnings)
