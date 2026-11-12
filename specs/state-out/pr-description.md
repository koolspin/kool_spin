# State out (Phase 1a, step 6b)

## Summary

After every block it renders, the engine now publishes each deck's state, the mixer's settings, and its own status (including what it has read from each client) through seqlocks in an engine-state block (`<ks/ipc/state.h>`). Each deck carries a raw visual play position: the track frame after the block, the host time at which it's due at the driver, and the block's output latency. The audio thread is the only writer and never waits for a reader.

To get there, tracks gained a 64-bit ID, blocks an output latency, and the render core gained render timing (blocks rendered, late blocks, render time, load, peak load), a readable mute flag, the output state (moved from the output controller), and a `Graph::rendered()` hook called once per rendered block. The engine implements the hook.

Spec: `specs/state-out/spec.md`. Plan with per-pass notes: `specs/state-out/plan.md`. Built by the loop in five passes plus this finishing pass.

## Checks

On Linux, every one of these passes in full, acceptance tests included:

- `cmake --workflow --preset debug`: 245 of 245 tests (123 acceptance);
- `cmake --workflow --preset asan`: 245 of 245;
- `cmake --workflow --preset rtsan`: 247 of 247 (the extra two are rtsan guardrails);
- `cmake --workflow --preset tsan`: 245 of 245, with no ThreadSanitizer report;
- `tools/lint.sh`: clean.

No "Stub" comment or stub `NOLINT` is left in `src/engine`. The public API in the headers is exactly what the approved spec commit declared; the loop added only private members and helpers.

## What was built, task by task

1. **Track IDs and output latency** (requirements 9 to 11). `Track` stores its ID; `render_interleaved()` gives its `output_latency` to every block it splits a callback into. The paced, offline, and miniaudio drivers rely on the default of 0 and didn't change. Unit test: "A device callback's output latency reaches every block it splits into".
2. **The render core** (requirements 12 to 15). `try_render()` reads `monotonic_now()` once the driver check passes and again after the safety stage, stores every count (`silenced_` still last), builds a `RenderStats` from the same values the accessors return, and calls `graph_->rendered()` before releasing the token. Loads are stored as a double's bit pattern in `std::atomic<std::uint64_t>`. Unit tests: the hook skips skipped and rejected blocks; timing reads 0 before the first block; mute and output state are kept.
3. **The output controller sets the core's output state** (requirement 14). A private `set_state()` assigns the controller's state and calls `core_->set_output_state()`; every transition goes through it. Unit test: "The core's output state follows every controller transition" (Playing → Freewheeling → Attaching → Freewheeling → Playing → Stopped).
4. **The engine publishes** (requirements 16 to 18, and 5 and 6's `clients`). The state-block constructor delegates to `Engine{map}` and emplaces one `SeqlockWriter` per deck, one for the mixer, and one for the status, in `std::optional`s. `rendered()` returns at once without them; otherwise it computes the block's end once and writes every deck, then the mixer, then the status, each built in a value-initialized local and written once. Encoders (reals, host times, durations, side, output state, rejection codes) are `noexcept KS_NONBLOCKING` functions in an anonymous namespace, using switches rather than casts for the codes. Unit tests: a state block doesn't change the output; each rendered block writes every seqlock once and a skipped one nothing; a deck's block describes its track; the status acknowledges each client slot.
5. **Acknowledgements, concurrency, real time** (requirements 19 and 20). No code change was needed (see Deviations); this pass confirmed "State: readers never see a torn block" under `tsan` and "State out is real-time safe" under `rtsan` and function-effect analysis.
6. **Finish.** Full workflows on all four Linux presets, lint, the stub check, the header doc check, and this description.

## How each `[auto]` criterion is covered

| Criterion | Test | Status |
|---|---|---|
| The layout | "State: the layout" | passes (passed before the loop) |
| Track ID | "Track: an ID" | passes |
| Output latency | "Render interleaved: the output latency reaches every block" | passes |
| The hook | "Render core: the graph hears about each rendered block" | passes |
| Load | "Render core: load and late blocks" | passes |
| Output state | "Render core: the output state follows the controller" | passes |
| Each block publishes once | "State: each rendered block publishes everything once" | passes |
| Decks | "State: decks" | passes |
| Visual play position | "State: the visual play position" | passes |
| Mixer | "State: the mixer" | passes |
| Status | "State: status" | passes |
| Acknowledgements | "State: acknowledgements" | passes |
| Publishing changes nothing | "State: publishing doesn't change the output" | passes |
| Under load | "State: readers never see a torn block" | passes in every preset; `tsan` reports nothing |
| Real time | "State out is real-time safe" | passes; `rtsan` and function-effect analysis are clean |
| Every CI configuration | all of the above on macOS arm64 (`debug`, `release`, `asan`, `tsan`) | **only CI can check this**; build loops build on Linux only |

On macOS, remember that `tsan` doesn't report races in struct copies (CLAUDE.md), so the Linux `tsan` run is the real data-race check for the seqlock blocks.

## Deviations from the spec or plan

- **Acknowledgements were filled in task 4, not task 5.** The acceptance tests compare the whole state block after every block, so "State: decks", "State: the mixer", and "State: the visual play position" couldn't pass with `EngineStatus::clients` left zero, as task 4 planned. Task 5 then had only confirmation left. No spec deviation.
- **A block whose duration rounds to 0 ns gets a load of 0** instead of dividing by zero. This needs a sample rate above 1 GHz, so it can't happen in practice; the spec doesn't say what a zero duration's load is.
- No other deviations. `<ks/ipc/state.h>` wasn't touched.

## Decisions (from the plan)

- **Order:** track IDs and latency, then the render core (every engine test depends on its hook and stats), then the output controller, then the engine's publishing, with the concurrency and real-time checks last.
- **Doubles in atomics:** render load and peak are bit patterns in `std::atomic<std::uint64_t>`, lock-free on every platform and matching how the state segment encodes reals.
- **Timing points:** from just after the active-driver check to just after `safety_.process()`, so the hook's own time isn't counted. Restoring the floating-point mode falls inside and is negligible.
- **`stats.muted` and `stats.silenced`** come from the block's own snapshot of `muted_`, so they describe the block rendered.
- **Rejection codes:** `Rejection` starts at 0, so the published code is the enum value plus 1, with 0 for none.
- **The block's end:** decks and status both use `block.host_time + frames_to_duration(block.frames, block.sample_rate)`, the same expression as `rendered_until()`, so they match it bit for bit.
- **Writers in `std::optional`:** `SeqlockWriter` has no default constructor; `std::optional` keeps "no state block" simple, allocates nothing, and is filled on the control thread before the engine is shared with the audio thread.

## Learnings worth adding to `CLAUDE.md`

- **Acceptance tests that compare a whole published block force every field at once.** Splitting a plan by field groups (decks first, acknowledgements later) doesn't work when a test checks the entire struct after every block. Plans should group tasks by what the acceptance tests compare, not by field.
- **The loop's shell allowlist refuses `for` loops.** Run each preset as its own command.
- **Function-effect analysis was happy with `std::optional::operator->`, `std::bit_cast`, and `std::chrono` arithmetic** in `KS_NONBLOCKING` code; nothing had to be restructured.

## `[manual]` acceptance criteria

The spec has none. The one criterion the loop can't check is the CI one above: confirm the macOS arm64 jobs (`debug`, `release`, `asan`, `tsan`) pass on the pull request.
