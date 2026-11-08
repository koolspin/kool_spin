# kool_spin

DJ Software - 30 years later.

An open-source DJ application for Linux and macOS, designed so the music keeps playing no matter what fails. It's in early development; see the [roadmap](docs/roadmap.md) and the [design decisions](docs/adr/).

## Building

Requirements:

- CMake 3.28 or later, and Ninja
- [vcpkg](https://github.com/microsoft/vcpkg), with `VCPKG_ROOT` pointing at it
- Linux: Clang 21 (`sudo tools/llvm.sh 21 all` on Ubuntu and Debian)
- macOS: Xcode command-line tools

Configure, build, and run the tests in one step:

```sh
cmake --workflow --preset debug
```

| Preset | Purpose |
|---|---|
| `debug` | Day-to-day development |
| `release` | Optimized, with debug info for crash reports |
| `asan` | AddressSanitizer and UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer |
| `rtsan` | RealtimeSanitizer (Linux only) |
| `fuzz` | libFuzzer instrumentation (Linux only) |

`tools/lint.sh` checks formatting and runs clang-tidy.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
