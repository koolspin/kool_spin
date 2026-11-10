# ks::core: building blocks shared by every kool_spin process.

add_library(ks_core INTERFACE)
add_library(ks::core ALIAS ks_core)

target_sources(ks_core INTERFACE
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/ks/core/clock.h
        include/ks/core/realtime.h)

target_compile_features(ks_core INTERFACE cxx_std_23)
