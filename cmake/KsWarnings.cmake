# ks::warnings: compiler warnings for kool_spin's own targets. Link it PRIVATE
# into every target we build; third-party code is unaffected.

include(CheckCXXCompilerFlag)

option(KS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

add_library(ks_warnings INTERFACE)
add_library(ks::warnings ALIAS ks_warnings)

target_compile_options(ks_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion
    -Wcast-align -Wnull-dereference -Wformat=2 -Wimplicit-fallthrough
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual>
    $<$<BOOL:${KS_WARNINGS_AS_ERRORS}>:-Werror>)

# Function effect analysis checks KS_NONBLOCKING ([[clang::nonblocking]]) at
# compile time. Clang 20+ supports it but leaves it off by default.
foreach(flag IN ITEMS -Wfunction-effects -Wperf-constraint-implies-noexcept)
    string(MAKE_C_IDENTIFIER "KS_CXX_HAS${flag}" result_var)
    check_cxx_compiler_flag(${flag} ${result_var})
    if(${result_var})
        target_compile_options(ks_warnings INTERFACE $<$<COMPILE_LANGUAGE:CXX>:${flag}>)
    endif()
endforeach()
