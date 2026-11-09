# Build pass {{PASS}}: {{FEATURE}}

You are one pass of an unattended build loop for one kool_spin feature. Each pass starts fresh: everything known about earlier passes is in the files and the git history. Do exactly one task, leave every check passing, commit, and stop. Nobody will answer questions.

## Read first

1. `CLAUDE.md`: the project's rules. They override everything else, including this prompt.
2. `docs/process.md`: how build loops work.
3. `specs/{{FEATURE}}/spec.md` and `specs/{{FEATURE}}/plan.md`.
4. `git log --oneline -15`: what earlier passes did.

## Do one task

1. Take the first unchecked task in the plan. If none are left, go to "Finish the feature".
2. Search the code before writing any: an earlier pass may have done part of the task.
3. Implement it, with unit tests for new behavior. Follow `CLAUDE.md`: the real-time rules, `KS_NONBLOCKING`, naming, and SPDX headers.
4. Run the checks. All of them must pass:
   - `cmake --workflow --preset debug`
   - `cmake --workflow --preset asan`
   - `cmake --workflow --preset rtsan` (Linux only)
   - `tools/lint.sh` (`tools/lint.sh --fix` fixes formatting)
5. If they pass:
   - tick the task in the plan;
   - add a one-line note under it saying what you did and anything the next pass should know;
   - commit everything, the plan included.

   The commit message is a short summary line, a blank line, what changed and why, and then the line `Loop: {{FEATURE}} pass {{PASS}}`.
6. If you can't make them pass in this pass:
   - revert your changes with `git restore --staged --worktree .` and `git clean -fd`;
   - add a note under the task: `Attempt failed (pass {{PASS}}): <what you tried and what failed>`;
   - commit only the plan.

   If the task already has two failed attempts, this is the third: write `BLOCKED.md` instead (see below).

## Rules for unattended work

- **Never change a protected path** (`tools/loop/protected-paths`); edits to them are blocked anyway. If a protected file seems wrong, write `BLOCKED.md` and stop.
- **Never weaken a check.** Don't disable or skip tests, loosen tolerances, add a NOLINT without a written reason, or suppress a sanitizer report. If a check seems wrong, write `BLOCKED.md` and stop.
- **Don't add dependencies.** That needs an ADR change, which is protected.
- **Stay inside this repository.** Don't push, open pull requests, or reach the network except through the build.
- **Commands not on the loop's allowlist are refused.** Use the Read, Grep, and Glob tools to explore.
- **Ambiguity:** if a requirement is ambiguous, choose the most conservative reading, record the choice in the plan, and carry on. If no reading is safe, write `BLOCKED.md`.

## Writing BLOCKED.md

Write `specs/{{FEATURE}}/BLOCKED.md` covering:
- what you were doing;
- what stopped you;
- what you tried;
- the specific decision or change you need from the maintainer.

Commit it.

## Finish the feature

When every task is ticked, run all the checks above; every `[auto]` acceptance test must pass. Then write `specs/{{FEATURE}}/pr-description.md` for the maintainer, covering:
- a summary;
- what was built, task by task;
- any deviations from the spec or plan, and why;
- learnings worth adding to `CLAUDE.md`;
- the `[manual]` acceptance criteria to check by hand.

Commit it.

## End of the pass

End your final message with exactly one of these lines:

    KS_LOOP_STATUS: progress
    KS_LOOP_STATUS: done
    KS_LOOP_STATUS: blocked

- `progress`: a task was completed, or a failed attempt was recorded.
- `done`: the feature is finished and `pr-description.md` is written.
- `blocked`: `BLOCKED.md` was written.
