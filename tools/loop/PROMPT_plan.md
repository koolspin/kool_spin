# Planning pass: {{FEATURE}}

You are planning one kool_spin feature in a single, unattended run. Nobody will answer questions. Write the plan, commit it, and stop.

## Read first

1. `CLAUDE.md`: the project's rules. They override everything else, including this prompt.
2. `docs/process.md`: how build loops work.
3. `specs/{{FEATURE}}/spec.md`: the requirements and acceptance criteria.
4. `tests/acceptance/{{FEATURE}}/`: the acceptance tests the build must make pass. They're protected; you can't change them. They have the CTest label `acceptance` and fail until the feature is built: `ctest --preset debug -L acceptance` shows where things stand (configure and build `debug` first).
5. The ADRs in `docs/adr/` that the spec refers to.
6. The existing code the feature touches. Search before assuming something doesn't exist.

## Write `specs/{{FEATURE}}/plan.md`

Start from `specs/_template/plan.md`. The plan is an ordered list of tasks:

- **One task per build pass:** a single focused change, with its tests, that leaves every check passing. If a task looks bigger than about 200 changed lines, split it.
- **Order:** each task builds on the ones before it, and acceptance tests start passing as early as possible.
- **Traceability:** each task names the acceptance criteria it advances.
- **No protected paths:** plan no changes to anything listed in `tools/loop/protected-paths`. If the feature can't be built without one (a new dependency in `vcpkg.json`, or a change to an ADR, `CLAUDE.md`, a spec, or an acceptance test), don't write a plan. Write `specs/{{FEATURE}}/BLOCKED.md` instead, saying what needs to change and why.

Write no code in this pass.

## Finish

Commit the plan (or `BLOCKED.md`): `git add specs/{{FEATURE}}/` then `git commit`. End the commit message with the line `Loop: {{FEATURE}} plan`.

Then end your final message with exactly one of these lines:

    KS_LOOP_STATUS: progress
    KS_LOOP_STATUS: blocked

Use `progress` when you wrote the plan, and `blocked` when you wrote `BLOCKED.md`.
