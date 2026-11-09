#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Guardrail: proves the protected-path checks work. Covers the Claude Code hook
# that stops build loops from editing protected files, and the CI check that
# fails a pull request changing them without the maintainer's approval.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
check="$root/tools/loop/check-protected.sh"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
failures=0

report() { # report <ok?> <description> [detail]
    if [[ "$1" == yes ]]; then
        echo "ok    $2"
    else
        echo "FAIL  $2 ${3:-}"
        failures=$((failures + 1))
    fi
}

# expect_hook <expected exit code> <description> <hook input JSON>
expect_hook() {
    local expected="$1" what="$2" json="$3" actual=0
    CLAUDE_PROJECT_DIR="$root" "$check" --hook <<<"$json" >/dev/null 2>&1 || actual=$?
    report "$([[ "$actual" == "$expected" ]] && echo yes || echo no)" "$what" "(exit $actual, expected $expected)"
}

edit_input() { # edit_input <file_path>
    jq -n --arg path "$1" --arg cwd "$root" \
        '{hook_event_name: "PreToolUse", tool_name: "Edit", cwd: $cwd, tool_input: {file_path: $path}}'
}

echo "== hook: blocks protected files"
expect_hook 2 "CLAUDE.md" "$(edit_input "$root/CLAUDE.md")"
expect_hook 2 "an ADR" "$(edit_input "$root/docs/adr/0001-tech-stack.md")"
expect_hook 2 "a new acceptance test" "$(edit_input "$root/tests/acceptance/demo/new_test.cpp")"
expect_hook 2 "a guardrail test" "$(edit_input "$root/tests/guardrails/rtsan_canary.cpp")"
expect_hook 2 "an approved spec" "$(edit_input "$root/specs/demo/spec.md")"
expect_hook 2 "the loop's own settings" "$(edit_input "$root/tools/loop/settings.json")"
expect_hook 2 "the sanitizer presets" "$(edit_input "$root/CMakePresets.json")"
expect_hook 2 "a CI workflow" "$(edit_input "$root/.github/workflows/ci.yml")"
expect_hook 2 "a relative path" "$(edit_input "CLAUDE.md")"
expect_hook 2 "a path that climbs back in with .." "$(edit_input "$root/src/../CLAUDE.md")"
ln -s "$root/CLAUDE.md" "$scratch/link"
expect_hook 2 "a symlink outside the project that points to a protected file" "$(edit_input "$scratch/link")"
expect_hook 2 "a protected notebook (NotebookEdit)" \
    "$(jq -n --arg p "$root/docs/adr/x.ipynb" '{tool_name: "NotebookEdit", tool_input: {notebook_path: $p}}')"
expect_hook 2 "anything, when the hook input isn't valid JSON (fails closed)" "not json"

echo "== hook: allows everything else"
expect_hook 0 "source code" "$(edit_input "$root/src/core/include/ks/core/realtime.h")"
expect_hook 0 "an ordinary test" "$(edit_input "$root/tests/tone/tone_generator_test.cpp")"
expect_hook 0 "a feature's plan" "$(edit_input "$root/specs/demo/plan.md")"
expect_hook 0 "a feature's BLOCKED.md" "$(edit_input "$root/specs/demo/BLOCKED.md")"
expect_hook 0 "a file outside the project" "$(edit_input "$scratch/elsewhere.txt")"

echo "== diff: the CI check"
repo="$scratch/repo"
git init -q -b main "$repo"
commit() { git -C "$repo" add -A && git -C "$repo" -c user.name=test -c user.email=test@example.com \
    -c commit.gpgsign=false commit -q -m "$1"; }
mkdir -p "$repo/src"
echo base >"$repo/src/a.cpp"
echo rules >"$repo/CLAUDE.md"
commit base
git -C "$repo" checkout -q -b feature

# expect_diff <expected exit code> <description> [approved]
expect_diff() {
    local actual=0
    (cd "$repo" && KS_PROTECTED_APPROVED="${3:-false}" "$check" --diff main) >/dev/null 2>&1 || actual=$?
    report "$([[ "$actual" == "$1" ]] && echo yes || echo no)" "$2" "(exit $actual, expected $1)"
}

echo changed >"$repo/src/a.cpp"
commit "change source"
expect_diff 0 "passes when only unprotected files change"

echo "weakened" >>"$repo/CLAUDE.md"
commit "change rules"
expect_diff 1 "fails when a protected file changes"
expect_diff 0 "passes when the maintainer approved" true

git -C "$repo" checkout -q -b rename main
git -C "$repo" mv CLAUDE.md NOTES.md
commit "rename rules away"
expect_diff 1 "fails when a protected file is renamed away"

echo
if ((failures > 0)); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
