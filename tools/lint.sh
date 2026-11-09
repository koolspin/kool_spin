#!/usr/bin/env bash
# Checks formatting with clang-format and runs clang-tidy on kool_spin's own sources.
#
# Usage: tools/lint.sh [--fix] [build-dir]
#   --fix       reformat files in place instead of only checking them
#   build-dir   a configured preset build directory (default: build/debug);
#               clang-tidy reads its compile_commands.json.
#
# CLANG_FORMAT, CLANG_TIDY, and RUN_CLANG_TIDY override the tool paths.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fix=0
if [[ "${1:-}" == "--fix" ]]; then
    fix=1
    shift
fi
build_dir="${1:-$root/build/debug}"

# Prefer the pinned LLVM version (apt.llvm.org names), then unversioned names (Homebrew).
pick() {
    local name
    for name in "$@"; do
        if command -v "$name" >/dev/null 2>&1; then
            echo "$name"
            return
        fi
    done
    echo "error: none of $* found" >&2
    exit 1
}
clang_format="${CLANG_FORMAT:-$(pick clang-format-21 clang-format)}"
clang_tidy="${CLANG_TIDY:-$(pick clang-tidy-21 clang-tidy)}"
run_clang_tidy="${RUN_CLANG_TIDY:-$(pick run-clang-tidy-21 run-clang-tidy)}"

cd "$root"
sources=('src/*.cpp' 'src/*.h' 'tests/*.cpp' 'tests/*.h' 'fuzz/*.cpp' 'fuzz/*.h')

# Tracked files plus new files not yet added (but not ignored ones).
list_sources() {
    git ls-files -z --cached --others --exclude-standard -- "${sources[@]}"
}

# Run every check, then fail if any of them failed, so one run reports all problems.
status=0

echo "== clang-format ($clang_format)"
if ((fix)); then
    list_sources | xargs -0 "$clang_format" -i || status=1
else
    list_sources | xargs -0 "$clang_format" --dry-run --Werror || status=1
fi

echo "== clang-tidy ($clang_tidy)"
if [[ ! -f "$build_dir/compile_commands.json" ]]; then
    echo "error: $build_dir/compile_commands.json not found; configure a preset first" >&2
    exit 1
fi
"$run_clang_tidy" -quiet -p "$build_dir" -clang-tidy-binary "$(command -v "$clang_tidy")" \
    "$root/(src|tests|fuzz)/" || status=1

if [[ $status -ne 0 ]]; then
    echo "lint: FAILED" >&2
else
    echo "lint: OK"
fi
exit $status
