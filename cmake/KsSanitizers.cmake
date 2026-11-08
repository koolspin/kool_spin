# Instruments the whole build with one sanitizer, chosen by KS_SANITIZER.
# The sanitizer presets in CMakePresets.json set it.

set(KS_SANITIZER "" CACHE STRING "Sanitizer to build with: asan, tsan, rtsan, fuzz, or empty for none")
set_property(CACHE KS_SANITIZER PROPERTY STRINGS "" asan tsan rtsan fuzz)

set(_ks_sanitizer_flags "")
if(KS_SANITIZER STREQUAL "asan")
    # AddressSanitizer plus UndefinedBehaviorSanitizer; any UB report fails the run.
    set(_ks_sanitizer_flags -fsanitize=address,undefined -fno-sanitize-recover=all)
elseif(KS_SANITIZER STREQUAL "tsan")
    set(_ks_sanitizer_flags -fsanitize=thread)
elseif(KS_SANITIZER STREQUAL "rtsan")
    # RealtimeSanitizer: reports blocking calls made inside KS_NONBLOCKING functions.
    set(_ks_sanitizer_flags -fsanitize=realtime)
elseif(KS_SANITIZER STREQUAL "fuzz")
    # Coverage instrumentation for libFuzzer, plus ASan/UBSan. Fuzz targets add
    # -fsanitize=fuzzer when they link.
    set(_ks_sanitizer_flags -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all)
elseif(NOT KS_SANITIZER STREQUAL "")
    message(FATAL_ERROR "Unknown KS_SANITIZER '${KS_SANITIZER}': expected asan, tsan, rtsan, fuzz, or empty")
endif()

if(_ks_sanitizer_flags)
    add_compile_options(${_ks_sanitizer_flags} -fno-omit-frame-pointer)
    add_link_options(${_ks_sanitizer_flags})
endif()
