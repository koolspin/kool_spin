# Chain-loaded by vcpkg's toolchain file (VCPKG_CHAINLOAD_TOOLCHAIN_FILE in
# CMakePresets.json) to choose the compiler before project() runs.
#
# Linux: Clang ${KS_CLANG_VERSION} from apt.llvm.org (tools/llvm.sh), which only
# installs versioned binaries such as clang++-21. Pinning one version keeps
# local builds and CI identical.
# macOS: the default AppleClang.
#
# An explicit -DCMAKE_CXX_COMPILER=... still takes precedence.

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CXX_COMPILER)
    set(KS_CLANG_VERSION 21 CACHE STRING "Clang major version used on Linux")
    find_program(KS_CLANG_C NAMES clang-${KS_CLANG_VERSION} REQUIRED)
    find_program(KS_CLANG_CXX NAMES clang++-${KS_CLANG_VERSION} REQUIRED)
    set(CMAKE_C_COMPILER "${KS_CLANG_C}")
    set(CMAKE_CXX_COMPILER "${KS_CLANG_CXX}")
endif()
