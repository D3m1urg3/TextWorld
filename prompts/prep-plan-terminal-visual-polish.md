# Prompt: prep-plan for terminal visual polish

Paste the block below into a fresh session in `/Users/demiurge/Projects/TextWorld`.

---

/lore-development:prep-plan

Build an implementation plan for `.lore/work/specs/terminal-visual-polish.md`
(33 requirements, `REQ-POLISH-*`).

## Read first

- `.lore/work/specs/terminal-visual-polish.md` — the spec being planned
- `.lore/work/research/terminal-visual-polish-implementation.md` — linenoise
  choice, the verified background-colour bar trick, the 66-column number, why
  reverse video was dropped
- `.lore/work/brainstorm/terminal-visual-polish.md` — what is deliberately out of
  scope and why
- `.lore/work/plans/ai-resolver.md` — **the structural exemplar.** Match its
  granularity and its skeleton-first ordering. Ignore its badge formatting.

Source files the plan touches: `src/loop.cpp`, `src/render.cpp`, `src/band.cpp`,
`src/band.hpp`, `src/term.cpp`, `src/term.hpp`, `src/main.cpp`, `CMakeLists.txt`,
`tests/tests.cpp`, `seed/base.sql`.

## Plan shape

1. **Smallest atomic steps.** One file, one testable behaviour, one commit per
   step where that makes sense. Prefer more small steps to fewer large ones.
2. **A validation gate on every step** — the exact command or test that proves it
   done, deterministic wherever possible. The spec's AI Validation section has 20
   numbered checks; every one belongs to some step's gate.
3. **Deterministic work first.** Nothing here needs a live model, but the spinner
   (REQ-POLISH-25..28) is only observable with AI on, so put it last and treat it
   as the one step that spends tokens to verify.
4. **A requirement-to-step coverage map** at the end. All 33 `REQ-POLISH-*` IDs
   must appear.
5. **Mark only the risky steps.** MED if deterministic but concurrency-shaped or
   likely to need iteration; HIGH if it spends live-LLM tokens. Plain text, no
   badges, no up-front risk table. Most steps need no label. If a step comes out
   MED or HIGH, split it and say why.

## Ordering constraints that are not obvious from the spec

- **Vendoring linenoise (REQ-POLISH-20) changes `CMakeLists.txt` and the input
  loop.** Do it as its own step, early, so later steps are not debugging a build
  change and a layout change at once.
- **Line editing and the spinner are one piece of work, not two.** linenoise's
  multiplexing API (`linenoiseHide` / `linenoiseShow`) exists to print
  asynchronous output without wrecking the line being edited. Sequence the
  spinner after linenoise so it can use that API rather than hand-rolling
  cursor management.
- **`term.hpp` gains background-colour emission (REQ-POLISH-13) before the bars
  can be built (REQ-POLISH-8..12).** Two steps, not one: the pure `term.cpp`
  function with its own unit test, then the band spans that use it.
- **Deleting the duplicated `Exits:` / `You see:` lines (REQ-POLISH-5) and the
  band-failure fallback (REQ-POLISH-6, -6a) must land together.** The deletion
  makes the band the only route those facts reach the player. Splitting them
  leaves a commit where a band failure silently costs the exits.
- **The `look` change (REQ-POLISH-14..19) is the largest item.** It touches three
  `roomBlock` callers — `look`/`moved`, `examine` on a room, and
  `renderStartup`. Break it up by caller if that yields cleaner gates.

## Things already verified — do not re-derive

- `band.cpp:232` and `render.cpp:70` run byte-identical exit queries, including
  the `architectEnabled()` gate. REQ-UI-10 says so explicitly. The deletion in
  REQ-POLISH-5 is safe.
- `\x1b[41m      \x1b[0m` measures 6 code points after `stripSgr`. Background-colour
  bars need no change to `layoutBand`'s width arithmetic.
- `events(verb='moved', object=<destination room>)` is the row REQ-POLISH-15
  derives from. Respawn writes `downed`, not `moved` (`mutations.cpp:429`).
- `loop.cpp:193-194` wraps prose, then appends the band. That is the single
  composition site for REQ-POLISH-1 and REQ-POLISH-3.
- Upstream `antirez/linenoise` has UTF-8 and the async API; the forks are not
  needed. BSD-2, one `.c` file, ~1600 lines.

## Out of scope — do not plan steps for these

Room images, any second image vendor, streaming narration, a browser or native
window, per-room ASCII art beyond the title screen, the map, collapsing the band
when it is unchanged, and showing what the resolver decided.

## Two open questions the plan should carry, not settle

- Where the health bar sits in a hostile row, relative to the numbers and
  `[WINDING UP]`. Decide by looking at a real fight, not in the plan.
- Whether the title screen should be suppressible by an env switch. Not built
  until wanted.
