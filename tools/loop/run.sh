#!/usr/bin/env bash
# Runs an unattended build loop for one feature. See docs/process.md.
#
# Usage:
#   tools/loop/run.sh plan  <feature> [options]   one pass that writes specs/<feature>/plan.md
#   tools/loop/run.sh build <feature> [options]   one pass per plan task, until done or blocked
#
# Options:
#   --max-passes N    build passes before giving up (default 10)
#   --budget-usd X    spending cap for each pass, in US dollars (default 5)
#   --model NAME      model for every pass (default: Claude Code's default)
#
# Run it from a git worktree on the feature's branch, with a clean working tree.
# Each pass is a fresh Claude Code process; pass logs go to .loop/<feature>/.
#
# Exit codes: 0 done, 1 error, 2 blocked (read specs/<feature>/BLOCKED.md),
#             3 pass limit reached, 64 usage.
set -euo pipefail

say() { echo "loop: $*"; }
die() {
    echo "loop: error: $*" >&2
    exit 1
}
usage() {
    sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 64
}

# ---- Arguments --------------------------------------------------------------

[[ $# -ge 2 ]] || usage
mode="$1"
feature="$2"
shift 2
max_passes=10
budget=5
model=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --max-passes) [[ $# -ge 2 ]] || usage; max_passes="$2"; shift 2 ;;
        --budget-usd) [[ $# -ge 2 ]] || usage; budget="$2"; shift 2 ;;
        --model) [[ $# -ge 2 ]] || usage; model="$2"; shift 2 ;;
        *) usage ;;
    esac
done
[[ "$mode" == plan || "$mode" == build ]] || usage
[[ "$feature" =~ ^[a-z0-9][a-z0-9-]*$ ]] || die "feature names use lowercase letters, digits, and '-': '$feature'"
[[ "$max_passes" =~ ^[1-9][0-9]*$ ]] || die "--max-passes takes a positive whole number"
[[ "$budget" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "--budget-usd takes a number"

# ---- Preconditions ----------------------------------------------------------

root="$(git rev-parse --show-toplevel 2>/dev/null)" || die "not inside a git repository"
cd "$root"
spec_dir="specs/$feature"
log_dir=".loop/$feature"

[[ -f "$spec_dir/spec.md" ]] || die "no spec at $spec_dir/spec.md"
branch="$(git symbolic-ref --quiet --short HEAD)" || die "HEAD is detached: check out the feature's branch"
case "$branch" in
    main | master) die "refusing to run on '$branch': use the feature's branch, in a worktree (docs/process.md)" ;;
esac
[[ -z "$(git status --porcelain)" ]] || die "the working tree has uncommitted changes"
if [[ "$(git rev-parse --absolute-git-dir)" == "$(git rev-parse --path-format=absolute --git-common-dir)" ]]; then
    say "warning: this is the main checkout; docs/process.md recommends running loops in a worktree"
fi
[[ -n "${VCPKG_ROOT:-}" ]] || die "VCPKG_ROOT is not set"
for tool in claude jq git python3; do
    command -v "$tool" >/dev/null || die "'$tool' not found"
done
if [[ "$(uname -s)" == Linux ]]; then
    for tool in bwrap socat; do
        command -v "$tool" >/dev/null || die "the sandbox needs '$tool': sudo apt install bubblewrap socat"
    done
    # Ubuntu 24.04 and later block the user namespaces bubblewrap needs, unless an
    # AppArmor profile allows them. Check now, rather than fail inside the first pass.
    if ! bwrap --ro-bind / / --dev /dev --unshare-all --die-with-parent /bin/true 2>/dev/null; then
        die "bubblewrap can't create a sandbox here; on Ubuntu, add an AppArmor profile for it (see 'Prerequisites' in docs/process.md)"
    fi
fi
if [[ -f "$spec_dir/BLOCKED.md" ]]; then
    say "blocked: resolve $spec_dir/BLOCKED.md, delete it, commit, and run again"
    exit 2
fi

# The loop's settings, plus write access for vcpkg's build trees in the sandbox.
settings="$(jq -c --arg vcpkg "$VCPKG_ROOT" '.sandbox.filesystem.allowWrite += [$vcpkg]' tools/loop/settings.json)"
mkdir -p "$log_dir"
total_cost=0
pass_status=""

# ---- One pass ---------------------------------------------------------------

render_prompt() { # render_prompt <plan|build> <pass number>
    sed -e "s/{{FEATURE}}/$feature/g" -e "s/{{PASS}}/$2/g" "tools/loop/PROMPT_$1.md"
}

# run_pass <plan|build> <pass number>: runs one Claude Code process and sets pass_status.
run_pass() {
    local kind="$1" pass="$2" stamp log result cost outcome started
    stamp="$(date +%Y%m%d-%H%M%S)"
    log="$log_dir/$stamp-$kind-$pass"
    local -a args=(
        -p "$(render_prompt "$kind" "$pass")"
        --settings "$settings"
        --setting-sources project # not your user settings: the loop gets only its own allowlist
        --strict-mcp-config       # no MCP servers
        --permission-mode dontAsk # anything not allowed is refused, never asked
        --output-format stream-json
        --verbose
        --max-budget-usd "$budget"
    )
    if [[ -n "$model" ]]; then
        args+=(--model "$model")
    fi

    say "$kind pass $pass: running (log: $log.jsonl)"
    started=$SECONDS
    # stdin from /dev/null: a pass never reads from, or waits on, the terminal.
    claude "${args[@]}" </dev/null >"$log.jsonl" 2>"$log.stderr" || true

    result="$(jq -rR 'fromjson? | select(.type == "result") | .result // ""' "$log.jsonl")"
    pass_status="$(grep -oE 'KS_LOOP_STATUS: (progress|done|blocked)' <<<"$result" | tail -n 1 | cut -d' ' -f2)" || true
    outcome="$(jq -rR 'fromjson? | select(.type == "result") | .subtype // ""' "$log.jsonl" | tail -n 1)"
    cost="$(jq -rR 'fromjson? | select(.type == "result") | .total_cost_usd // 0' "$log.jsonl" | tail -n 1)"
    cost="${cost:-0}"
    total_cost="$(awk -v a="$total_cost" -v b="$cost" 'BEGIN { printf "%.2f", a + b }')"
    say "$kind pass $pass: status '${pass_status:-none}' (${outcome:-no result}), $((SECONDS - started))s, \$$(printf '%.2f' "$cost"), total \$$total_cost"
}

check_clean() {
    if [[ -n "$(git status --porcelain)" ]]; then
        say "the pass left uncommitted changes; stopping so you can inspect them (git status)"
        exit 1
    fi
}

stop_if_blocked() {
    if [[ -f "$spec_dir/BLOCKED.md" ]]; then
        say "blocked after a total of \$$total_cost: read $spec_dir/BLOCKED.md"
        exit 2
    fi
}

# ---- Main -------------------------------------------------------------------

case "$mode" in
    plan)
        [[ ! -f "$spec_dir/plan.md" ]] || die "$spec_dir/plan.md already exists; delete it to plan again"
        run_pass plan 0
        check_clean
        stop_if_blocked
        [[ -f "$spec_dir/plan.md" ]] || die "the planning pass didn't write $spec_dir/plan.md; see $log_dir"
        say "plan written: review $spec_dir/plan.md, then run: tools/loop/run.sh build $feature"
        ;;

    build)
        [[ -f "$spec_dir/plan.md" ]] || die "no plan yet: run 'tools/loop/run.sh plan $feature' first"
        stalled=0
        for ((pass = 1; pass <= max_passes; pass++)); do
            before="$(git rev-parse HEAD)"
            run_pass build "$pass"
            check_clean
            git log --format='loop:   %h %s' "$before..HEAD"
            stop_if_blocked

            if [[ "$pass_status" == done ]]; then
                if grep -qE '^[[:space:]]*- \[ \]' "$spec_dir/plan.md"; then
                    say "the pass reported done, but the plan still has unchecked tasks; continuing"
                elif [[ ! -f "$spec_dir/pr-description.md" ]]; then
                    say "the pass reported done, but $spec_dir/pr-description.md is missing; continuing"
                else
                    say "done after $pass pass(es), total \$$total_cost."
                    say "Review the branch, push it, and open a pull request using $spec_dir/pr-description.md."
                    exit 0
                fi
            fi

            if [[ "$(git rev-parse HEAD)" == "$before" ]]; then
                stalled=$((stalled + 1))
            else
                stalled=0
            fi
            if ((stalled >= 3)); then
                cat >"$spec_dir/BLOCKED.md" <<EOF
# Blocked: no progress

The loop runner stopped after three build passes in a row that made no commit.
The pass logs in $log_dir show what happened. Resolve the problem, delete this
file, commit, and run the loop again.
EOF
                git add "$spec_dir/BLOCKED.md"
                git commit -q -m "Loop: $feature blocked after three passes without progress"
                stop_if_blocked
            fi
        done
        say "reached the pass limit ($max_passes) without finishing; total \$$total_cost"
        exit 3
        ;;
esac
