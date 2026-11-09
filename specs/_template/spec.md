# Feature: <name>

- **Status:** Draft. Once the maintainer approves it, this file is protected: build loops can't change it.
- **Roadmap:** Phase <n>
- **Built by:** loop | interactive

## Goal

One or two sentences: what the feature does, and why.

## Context

What already exists, which ADRs apply, and anything a reader starting from nothing needs to know.

## Requirements

Numbered, testable statements.

1. …

## Acceptance criteria

Tag each criterion `[auto]` or `[manual]`:
- `[auto]`: a test in `tests/acceptance/<feature>/` checks it; name the test case.
- `[manual]`: the maintainer checks it by hand.

A build loop is done when every `[auto]` criterion passes.

- [ ] `[auto]` … (test: `tests/acceptance/<feature>/<file>`, "<test case name>")
- [ ] `[manual]` …

## Out of scope

What not to build, even if it seems related.

## Open questions

Anything unresolved. A spec with open questions isn't ready for a loop.
