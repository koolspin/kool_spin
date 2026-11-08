// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file
/// Real-time safety annotations.
///
/// KS_NONBLOCKING marks a function as safe to run on the audio thread: it must
/// not allocate or free memory, lock, block, do I/O or system calls, or throw.
/// Mark the function `noexcept` too, and put KS_NONBLOCKING after it, which is
/// where C++ places attributes of a function's type:
///
///     void render(std::span<float> out) noexcept KS_NONBLOCKING;
///
/// With Clang 20 or later it expands to `[[clang::nonblocking]]`, which is
/// checked twice:
/// - at compile time, by function effect analysis (`-Wfunction-effects`),
///   which also rejects calls to functions that aren't themselves
///   nonblocking;
/// - at run time, by RealtimeSanitizer (the `rtsan` preset).
///
/// Other compilers ignore it. KS_HAS_NONBLOCKING is 1 when the checks are
/// active and 0 otherwise.

#ifdef __has_cpp_attribute
#if __has_cpp_attribute(clang::nonblocking)
#define KS_HAS_NONBLOCKING 1
#endif
#endif

#ifndef KS_HAS_NONBLOCKING
#define KS_HAS_NONBLOCKING 0
#endif

#if KS_HAS_NONBLOCKING
#define KS_NONBLOCKING [[clang::nonblocking]]
#else
#define KS_NONBLOCKING
#endif
