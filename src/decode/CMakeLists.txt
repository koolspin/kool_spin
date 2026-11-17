# ks::decode: decoding audio files with FFmpeg (specs/decode-worker/spec.md).
# Only the decode worker links it (ADR 0002). FFmpeg stays private: the
# public header includes nothing from it.

add_library(ks_decode STATIC
    decoder.cpp)
add_library(ks::decode ALIAS ks_decode)

target_sources(ks_decode PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/decode/decoder.h)

find_package(FFMPEG REQUIRED)
# FFmpeg's headers are someone else's code: no warnings from them.
target_include_directories(ks_decode SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(ks_decode
    PUBLIC ks::ipc
    PRIVATE ${FFMPEG_LIBRARIES} ks::warnings)
