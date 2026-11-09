# ks::dsp: signal-processing building blocks. Everything here is real-time safe.

add_library(ks_dsp INTERFACE)
add_library(ks::dsp ALIAS ks_dsp)

target_sources(ks_dsp INTERFACE
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/dsp/sine_oscillator.h)

target_link_libraries(ks_dsp INTERFACE ks::core)
