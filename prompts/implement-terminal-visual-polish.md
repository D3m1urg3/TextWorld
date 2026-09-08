# Prompt: implement terminal visual polish

Paste the block below into a fresh session in `/Users/demiurge/Projects/TextWorld`.

---

/lore-development:implement

Build `.lore/work/plans/terminal-visual-polish.md` — 21 steps implementing
`.lore/work/specs/terminal-visual-polish.md` (33 requirements, `REQ-POLISH-*`,
plus eight lettered sub-requirements).

The plan's status is `draft`. Flip it to `approved` before starting.

## Read first

- `.lore/work/plans/terminal-visual-polish.md` — the plan. Steps, validation
  gates, the requirement coverage map, and the check-to-step table. **This is
  the source of truth for what to build and in what order.**
- `.lore/work/specs/terminal-visual-polish.md` — the requirements themselves and
  the 20 numbered AI Validation checks the gates refer to
- `.lore/work/research/terminal-visual-polish-implementation.md` — the linenoise
  API surface, the verified bar-width measurement, why 66, why reverse video was
  dropped
- `.lore/work/plans/ai-resolver.md` — how a plan of this shape was executed last
  time

Read the brainstorm only if a scope question comes up; it is the record of what
is parked and why, not of what to build.

## Two spec amendments this plan carries

Both are decided, both are in the plan's second section, and **both need folding
back into the specs as part of the work** — not left as a plan-only note.

- **REQ-POLISH-15** must name the `meta.start_room` row. The derivation cannot
  work from `moved` events alone, because the starting room never gets one.
- **REQ-EXAMINE-7** (in `.lore/work/specs/`, not this spec) must say the player's
  own room is in scope for `examine`. Today `x cell` answers "You don't see that
  here." because a room has no `location` row. Update the comment at
  `systems.cpp:180` to match.

## Ordering that is not negotiable

- **Step 1 links linenoise into `twcore` as well as `textworld`.** `loop.cpp` is
  in `twcore`; `main.cpp` is not. A `textworld`-only link passes step 1's plain
  build and fails at step 19. The gate proves it with a throwaway call.
- **Step 5 does not split.** Deleting the `Exits:` / `You see:` lines makes the
  band the only route those facts reach the player, so the band-failure fallback
  lands in the same commit. Splitting them leaves a revision where a band failure
  silently costs the exits.
- **Steps 19 and 20 stay split.** 19 is the spinner's concurrency work, verified
  offline under `TEXTWORLD_AI=0`. 20 is three live turns and the only step that
  spends tokens. Do not merge them and do not run the suite with AI on.
- Steps 1, 10 and 15 depend on nothing and can move if that helps. Everything
  else runs in the order written.

## Things already verified — do not re-derive

- `band.cpp:232` and `render.cpp:70` run byte-identical exit queries, the
  `architectEnabled()` gate included. REQ-UI-10 says so. The deletion is safe.
- `\x1b[41m      \x1b[0m` measures 6 code points after `stripSgr`. Bars need no
  change to `layoutBand`'s width arithmetic.
- `roomBlock` has **four** callers, not three: `moved` (`render.cpp:118`),
  `looked` with a null detail (`:125`), `downed` (`:225`), and `renderRoomOf`
  (`:244`). `examined` is **not** one of them — it prints the `description` row
  directly at `:131`, which after step 5 is byte-identical to what `roomBlock`
  emits.
- Widening `TurnResult` (`loop.hpp:17`) is free: 37 mentions in `tests.cpp` and
  not one constructs it, so a defaulted third member breaks nothing.
- `wrapProse` measures with `utf8Length`, which counts escape bytes as columns.
  That is why refusals stay plain through `renderError` and are dimmed in
  `loop.cpp` after the wrap, and why `spells` output leaves the wrap path.
- Respawn writes `downed`, not `moved` (`mutations.cpp:429`), and
  `combat.cpp:609` hardcodes `kDormitoryCell` as the destination.
- Line numbers in the plan were checked against the tree on 2026-09-07. The
  spec's own `main.cpp` citations are stale; the plan's are right.

## Not verified — check before trusting

The ~25 `tests.cpp` assertion sites step 5 claims to touch (`:1900`, `:1909`,
`:2864`, `:2878`, `:4695-4701`, `:5261`, `:9439-9440`) and the `testBand*` line
numbers in later steps were read once while planning and never confirmed by a
second pass. Grep before editing.

## Golden literals

`kExamineGoldenSession` and `kStoryGoldenSession` get re-captured three times
over the run — at steps 5, 9 and 12. Use `TW_DUMP_GOLDEN=1` and `TW_DUMP_BANDS=1`
rather than hand-editing, and say in each commit message which step caused it.

`kStoryGoldenSession`'s comment says "Do NOT re-capture." That instruction is
aimed at the arc brick hiding a printed advance; REQ-POLISH-5 is exactly the
deliberate layout change spec check 20 exempts. If a **combat, turn-count, or
schema** test needs editing, that is a REQ-POLISH-32 failure — raise it, do not
edit it.

## Token discipline

Everything through step 18 runs offline with `TEXTWORLD_AI=0`. Step 20 is three
live turns with mechanical assertions; it is not a re-run of the suite with AI
on and it is not a place to iterate on anything.

Do not dispatch subagents. Keep `/implement`'s phase order, its per-step
validation gates, its task-file status updates and its notes file, and do the
work inline.

## Out of scope

Room images and any second image vendor; streaming narration; a browser or
native window; per-room ASCII art beyond the title screen; the map; collapsing
the band when it is unchanged; showing what the resolver decided.

## Two questions to answer while building, not before

- **Where the health bar sits in a hostile row**, relative to the numbers and
  `[WINDING UP]`. Step 9 puts it after `HP: n/m` so there is something to look
  at. Decide it by looking at a real fight.
- **Whether the title screen should be suppressible** by an env switch. Do not
  build one until it is wanted.
