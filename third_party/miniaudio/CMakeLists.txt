# ks::miniaudio: audio device I/O (miniaudio, public domain or MIT-0).
#
# The vcpkg port ships only the header; this target compiles the implementation
# once. kool_spin uses only the device API, so the decoders, encoders, and
# high-level engine are compiled out. The MA_NO_* definitions are PUBLIC because
# they must match between the implementation and every user of the header.

find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
find_package(Threads REQUIRED)

add_library(ks_miniaudio STATIC miniaudio.c)
add_library(ks::miniaudio ALIAS ks_miniaudio)

target_include_directories(ks_miniaudio SYSTEM PUBLIC "${MINIAUDIO_INCLUDE_DIR}")
target_compile_definitions(ks_miniaudio PUBLIC
    MA_NO_DECODING
    MA_NO_ENCODING
    MA_NO_GENERATION
    MA_NO_RESOURCE_MANAGER
    MA_NO_NODE_GRAPH
    MA_NO_ENGINE)
target_link_libraries(ks_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} $<$<PLATFORM_ID:Linux>:m>)

# miniaudio uses POSIX APIs that strict -std=c17 hides; build it as gnu17.
set_target_properties(ks_miniaudio PROPERTIES C_EXTENSIONS ON)

# ks::miniaudio_codecs: a second, private copy of miniaudio with its decoders
# and WAV encoder, for the development harness only (specs/harness/spec.md
# requirements 30 and 31). Every miniaudio symbol in it is static, so it links
# beside ks::miniaudio; users see only ks_codecs.h, never miniaudio.h's
# codec declarations. The engine must never link it.
add_library(ks_miniaudio_codecs STATIC codecs.c)
add_library(ks::miniaudio_codecs ALIAS ks_miniaudio_codecs)

target_include_directories(ks_miniaudio_codecs
    PRIVATE "${MINIAUDIO_INCLUDE_DIR}"
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ks_miniaudio_codecs PUBLIC Threads::Threads $<$<PLATFORM_ID:Linux>:m>)
set_target_properties(ks_miniaudio_codecs PROPERTIES C_EXTENSIONS ON)
