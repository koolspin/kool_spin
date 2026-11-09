# Development process

kool_spin is built AI-first, in two ways:

- **Interactively:** the maintainer works with Claude Code in a session, reviewing each step. Used for risky core code (real-time and lock-free code, shared memory, crash recovery), for design, and for writing specs and acceptance tests.
- **Build loops:** unattended Claude Code runs that implement a well-specified feature one task at a time, checked by tests the maintainer has already approved. Used for breadth work with a clear "done".

Each phase in the [roadmap](roadmap.md) says which parts are built which way.

## The workflow for one feature

| Step | Who | Produces |
|---|---|---|
| 1. Spec | Maintainer and Claude, interactively | `specs/<feature>/spec.md`, from `specs/_template/spec.md` |
| 2. Acceptance tests | Claude, interactively; the maintainer reviews | `tests/acceptance/<feature>/`, one test for each `[auto]` criterion |
| 3. Plan | A loop, in planning mode (one pass) | `specs/<feature>/plan.md`, which the maintainer skims |
| 4. Build | A loop, in build mode (one pass per task) | Commits, each leaving every check passing |
| 5. Handoff | The loop's last pass | `specs/<feature>/pr-description.md` |
| 6. Review | Maintainer | Checks the `[manual]` criteria, pushes, opens the pull request, merges |

Steps 1 and 2 are committed on the feature's branch before any loop runs. Once they're approved, the spec and the acceptance tests are protected: a loop can't change what "done" means.

## Files for a feature

```
specs/<feature>/
  spec.md              requirements and acceptance criteria (protected)
  plan.md              the loop's task list, ticked off pass by pass
  BLOCKED.md           only when the loop is stuck: what it needs from the maintainer
  pr-description.md    the loop's handoff
tests/acceptance/<feature>/   acceptance tests (protected)
```

## Running a loop

### Prerequisites

- **Build environment:** the project builds locally, with `VCPKG_ROOT` set.
- **Sandbox:** on Linux, `bubblewrap` and `socat` (`sudo apt install bubblewrap socat`). macOS has its sandbox built in. The loop refuses to run without the sandbox.

### Start

Loops run in their own git worktree, so they never touch your main checkout and you can keep working while one runs:

```sh
git worktree add ../kool_spin-<feature> feature/<feature>
cd ../kool_spin-<feature>

tools/loop/run.sh plan <feature>      # one pass: writes and commits specs/<feature>/plan.md
# review the plan
tools/loop/run.sh build <feature>     # one pass per task, until done or blocked
```

Run it from a normal terminal, not from inside a Claude Code session. Use tmux if it should survive closing the terminal.

**Options:**
- `--max-passes N`: pass limit (default 10).
- `--budget-usd X`: spending cap for each pass (default 5).
- `--model NAME`: model for every pass.

### What happens in a pass

Each pass is a fresh `claude -p` process. It has no memory of earlier passes: the spec, the plan, and the git history are the only state. A build pass:

1. takes the first unchecked task in the plan;
2. implements it with tests;
3. runs the checks;
4. ticks the task and commits.

If it can't make the checks pass, it reverts its changes and records the failed attempt in the plan instead.

**Checks in a pass, and at the end:** acceptance tests describe the finished feature, so most fail until the last task is done. Every test in `tests/acceptance/` gets the CTest label `acceptance`, which splits the checks:
- **Each pass:** every other test must pass in the `debug`, `asan`, and `rtsan` presets (`ctest -LE acceptance`), lint must be clean, and no acceptance test that passed before the pass may fail after it.
- **Finishing:** the final pass, and CI on the pull request, run the complete suite, and everything must pass.

A feature's acceptance tests register themselves: `tests/acceptance/CMakeLists.txt` adds every subdirectory that has a `CMakeLists.txt`, and provides `ks_acceptance_test()`, which checks a command's exit code and output lines with `tests/acceptance/expect.sh`. The prompts are `tools/loop/PROMPT_plan.md` and `tools/loop/PROMPT_build.md`. When a loop misbehaves, fix the prompt rather than patching its output by hand.

### When it stops

| Exit code | Meaning | What to do |
|---|---|---|
| 0 | Done: every task ticked, `pr-description.md` written | Review, check the `[manual]` criteria, push, open the pull request |
| 2 | Blocked: `specs/<feature>/BLOCKED.md` | Resolve it (change the spec, approve a protected change, or fix the environment), delete `BLOCKED.md`, commit, and run again |
| 3 | Pass limit reached | Read the plan and the logs; run again with a higher `--max-passes` if it's making progress |
| 1 | Error, or a pass left uncommitted changes | Read the message; `git status` |

**When a loop blocks itself:**
- **Three failed attempts:** a pass writes `BLOCKED.md` after the third failed attempt at the same task.
- **No progress:** the runner writes it after three passes in a row that made no commit.

**"done" isn't taken on trust.** The runner accepts it only when the plan has no unchecked tasks and the handoff exists. CI then rebuilds and retests everything in isolation when the branch is pushed. The runner doesn't rerun the tests itself, because that would execute loop-written code outside the sandbox.

### Logs and cost

Every pass logs its full transcript to `.loop/<feature>/<time>-<kind>-<pass>.jsonl`, with stderr in a matching `.stderr` file. The `.loop/` directory is gitignored. After each pass the runner prints the pass's status, duration, and cost, plus the running total.

## Safety

No single mechanism is airtight, so several independent layers each limit what a loop can do:

| Layer | What it does | Where |
|---|---|---|
| Protected-path hook | Blocks the file-editing tools from changing protected paths. If the check itself fails, it blocks rather than allows. | `tools/loop/check-protected.sh`, registered in `tools/loop/settings.json` |
| Allowlist | `--permission-mode dontAsk`: any tool or command not explicitly allowed is refused. There's no `git push`, network tools, or `sudo`. | `tools/loop/settings.json` |
| Isolated settings | Only the loop's settings and the project's settings load (not your personal settings), and no MCP servers or claude.ai connectors | `tools/loop/run.sh` |
| Sandbox | Bash commands can write only to the worktree, the temporary directory, and vcpkg's directories, and reach only GitHub on the network. The loop fails to start without the sandbox. | `tools/loop/settings.json` |
| Write confinement | The file-editing tools can't write outside the worktree | `blockWritesOutsideWorkingDirectories` |
| CI check | A pull request that changes a protected path fails until the maintainer adds the `protected-paths-approved` label | `.github/workflows/protected-paths.yml` |
| Guardrail tests | Prove the hook and the CI check work | `tests/guardrails/protected_paths_test.sh` |

**Known gap:** a command on the allowlist could still write a protected file through shell redirection (`>`). The hook doesn't see Bash commands, so the CI check is the backstop.

## Protected paths

`tools/loop/protected-paths` lists them:
- acceptance, guardrail, and golden tests;
- build, warning, sanitizer, and lint configuration;
- `vcpkg.json`;
- `CLAUDE.md`, the ADRs, this file, the roadmap, approved specs, and the spec templates;
- the loop's own machinery, Claude Code settings, and CI.

They protect what defines "done", the checks themselves, and the decisions the maintainer owns. A loop that needs one of them changed has to stop and ask, through `BLOCKED.md`.

**Interactive sessions aren't blocked.** The hook is only in the loop's settings, but any pull request that touches a protected path still needs the `protected-paths-approved` label, whoever made the change. The maintainer adds the label after reviewing those files.

## Tips

- **CI logs:** `gh run view --log` returns nothing for jobs whose names contain `/`, such as `Linux / rtsan`. Fetch a job's log with `gh api repos/koolspin/kool_spin/actions/jobs/<job-id>/logs` instead.
- **Learnings:** loops record learnings in the plan and the handoff rather than in `CLAUDE.md`, which is protected. The maintainer moves anything worth keeping into `CLAUDE.md`.
