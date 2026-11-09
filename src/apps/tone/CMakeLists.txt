# ks-tone: plays a test tone, to check audio output end to end.

# The library holds everything except argument handling, so tests can use it.
add_library(ks_tone STATIC
    options.cpp
    tone_generator.cpp
    tone_player.cpp)
add_library(ks::tone ALIAS ks_tone)

target_sources(ks_tone PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/tone/options.h
        include/ks/tone/tone_generator.h
        include/ks/tone/tone_player.h)

target_link_libraries(ks_tone
    PUBLIC ks::core ks::dsp
    PRIVATE ks::miniaudio ks::warnings)

add_executable(ks-tone main.cpp)
target_link_libraries(ks-tone PRIVATE ks::tone ks::warnings)
