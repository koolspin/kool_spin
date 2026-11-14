#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs a command and checks its exit code and output, for acceptance tests.
#
# Usage: expect.sh <exit code> [<pattern>...] -- <command> [<argument>...]
#
# <exit code> is one code, or several separated by '|', such as 0|3: the
# command may exit with any of them.
#
# Each <pattern> is an extended regular expression (grep -E) that must match at
# least one line of the command's combined stdout and stderr. A pattern that
# starts with '!' must match no line.
set -uo pipefail

expected_exit="$1"
shift
patterns=()
while [[ $# -gt 0 && "$1" != "--" ]]; do
    patterns+=("$1")
    shift
done
if [[ "${1:-}" != "--" || $# -lt 2 ]]; then
    echo "expect.sh: usage: expect.sh <exit code> [<pattern>...] -- <command> [<argument>...]" >&2
    exit 64
fi
shift

output="$("$@" 2>&1)"
actual_exit=$?
printf '%s\n---\n' "$output"

failed=0
if [[ "|$expected_exit|" != *"|$actual_exit|"* ]]; then
    echo "FAIL: exit code $actual_exit, expected $expected_exit"
    failed=1
fi
for pattern in ${patterns[@]+"${patterns[@]}"}; do
    if [[ "$pattern" == '!'* ]]; then
        if grep -Eq -- "${pattern:1}" <<<"$output"; then
            echo "FAIL: a line matches '${pattern:1}', and none may"
            failed=1
        fi
    elif ! grep -Eq -- "$pattern" <<<"$output"; then
        echo "FAIL: no line matches '$pattern'"
        failed=1
    fi
done
exit "$failed"
