cmake_minimum_required(VERSION 3.28)

project(kool_spin
    VERSION 0.0.1
    DESCRIPTION "Open-source DJ software"
    HOMEPAGE_URL "https://github.com/koolspin/kool_spin"
    LANGUAGES C CXX)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20)
    message(FATAL_ERROR
        "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old: [[clang::nonblocking]] and "
        "RealtimeSanitizer need Clang 20 or later. On Ubuntu: sudo tools/llvm.sh 21 all")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # no C++20 modules; skip the per-file dependency scan
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(KsSanitizers)
include(KsWarnings)

option(KS_BUILD_TESTS "Build the test suite" ON)

add_subdirectory(src)

if(KS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
