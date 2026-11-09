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
