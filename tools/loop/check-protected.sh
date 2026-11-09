#!/usr/bin/env bash
# Guards protected paths: files a build loop must not change. The patterns are
# in tools/loop/protected-paths; docs/process.md explains how they're used.
#
# Usage:
#   check-protected.sh --hook
#       Claude Code PreToolUse hook for file-editing tools. Reads the hook's
#       JSON on stdin and exits 2, which blocks the edit, if the target file is
#       protected. If the check itself fails, it blocks too (fails closed).
#   check-protected.sh --diff <base>
#       For CI. Lists protected files changed on HEAD since its merge base with
#       <base>. Exits 1 if there are any, unless KS_PROTECTED_APPROVED=true.
#   check-protected.sh --is-protected <path>
#       Exits 0 if the repository-relative <path> is protected, 1 if not.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
patterns_file="$script_dir/protected-paths"

usage() {
    sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 64
}

# is_protected <repository-relative path>
is_protected() {
    local path="$1" pattern
    while IFS= read -r pattern || [[ -n "$pattern" ]]; do
        case "$pattern" in '' | '#'*) continue ;; esac
        # shellcheck disable=SC2053 # unquoted on purpose: the pattern is a glob
        if [[ "$path" == $pattern ]]; then
            return 0
        fi
    done <"$patterns_file"
    return 1
}

# Absolute path with symlinks and '..' resolved; the file needn't exist yet.
canonical() {
    python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$1"
}

hook() {
    # Fail closed: if anything below fails unexpectedly, block the edit.
    trap 'echo "Blocked: the protected-path check (tools/loop/check-protected.sh) failed, so the edit was blocked to be safe." >&2; exit 2' ERR

    local input file_path cwd project abs rel
    input="$(cat)"
    file_path="$(jq -er '.tool_input.file_path // .tool_input.notebook_path // ""' <<<"$input")"
    if [[ -z "$file_path" ]]; then
        exit 0
    fi
    if [[ "$file_path" != /* ]]; then
        cwd="$(jq -r '.cwd // ""' <<<"$input")"
        file_path="${cwd:-$PWD}/$file_path"
    fi

    project="$(canonical "${CLAUDE_PROJECT_DIR:-$PWD}")"
    abs="$(canonical "$file_path")"
    case "$abs" in
        "$project"/*) rel="${abs#"$project"/}" ;;
        *) exit 0 ;; # outside the project: blockWritesOutsideWorkingDirectories applies
    esac

    if is_protected "$rel"; then
        trap - ERR
        cat >&2 <<EOF
Blocked: $rel is a protected path (see tools/loop/protected-paths and docs/process.md).
Build loops must not change it. If the task can't be done without changing it, write
specs/<feature>/BLOCKED.md explaining what needs to change and why, then stop.
EOF
        exit 2
    fi
    exit 0
}

diff_mode() {
    local base="$1" file
    local -a changed=()
    if ! git rev-parse --verify --quiet "${base}^{commit}" >/dev/null; then
        echo "error: '$base' is not a commit" >&2
        exit 1
    fi
    # --no-renames reports a rename as a deletion plus an addition, so moving a
    # protected file away still counts as changing it.
    while IFS= read -r file; do
        if is_protected "$file"; then
            changed+=("$file")
        fi
    done < <(git diff --name-only --no-renames "$base...HEAD")

    if ((${#changed[@]} == 0)); then
        echo "No protected paths changed."
        return 0
    fi
    echo "Protected paths changed:"
    printf '  %s\n' "${changed[@]}"
    if [[ "${KS_PROTECTED_APPROVED:-false}" == "true" ]]; then
        echo "Approved: the pull request has the 'protected-paths-approved' label."
        return 0
    fi
    local message="These files are protected (tools/loop/protected-paths). After reviewing the changes, a maintainer approves them by adding the 'protected-paths-approved' label to the pull request."
    if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then
        echo "::error title=Protected paths changed::$message"
    fi
    echo "error: $message" >&2
    return 1
}

case "${1:-}" in
    --hook) hook ;;
    --diff)
        [[ $# -eq 2 ]] || usage
        diff_mode "$2"
        ;;
    --is-protected)
        [[ $# -eq 2 ]] || usage
        is_protected "$2"
        ;;
    *) usage ;;
esac
