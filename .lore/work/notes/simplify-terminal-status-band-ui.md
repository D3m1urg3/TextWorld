---
title: "Simplify notes: terminal-status-band-ui"
date: 2026-08-02
status: complete
tags: [simplify, cleanup, notes, ui, terminal, status-band]
source: .lore/work/notes/terminal-status-band-ui.md
modules: [term, band]
related: [.lore/work/plans/terminal-status-band-ui.md, .lore/work/specs/terminal-status-band-ui.md]
---

# Simplify notes: terminal-status-band-ui

Files processed: `src/band.cpp`, `src/term.cpp`.

**Process deviation.** The skill dispatches `code-simplifier:code-simplifier`
via the Task tool; that agent is not registered in this session, and both files
had just been written in the same session, so a cold agent would have re-derived
context already in hand — the cost [[no-implement-subagents]] exists to avoid.
Done inline instead, with the full suite plus a real-binary byte comparison
standing in for the test and review dispatches.

## Behavior preservation

| Evidence | Before | After |
|---|---|---|
| Test suite | 5239 checks, 0 failures | 5239 checks, 0 failures |
| Build warnings/errors | 0 | 0 |
| `textworld` piped output (`look`, `quit`) | — | **byte-identical** to the pre-simplify run |

The byte comparison is the strong one: the seven golden bands already pin the
layout, and the binary diff pins the composed end-to-end output including the
color gate's decision.

## Changes

**1. `listRow()` — `exitsRow` and `objectsRow` were the same function twice.**
Both ran a query, collected strings, then built comma-separated spans with an
empty separator on the last item. The list-building half is now one helper; each
row function is its query plus one call. This also puts the "no dangling comma"
invariant in exactly one place, which matters because a trailing separator at a
line break was the width bug found during implementation.

**2. `styleSpan(BandSpan)` → `styleWord(text, color, bold, style)`.** The wrap
loop was constructing a throwaway `BandSpan` for every word purely to reach the
bold-vs-color rule. It now calls the rule directly with the three fields it
actually has.

**3. `bolden()` defined via `boldColor()`.** `bolden(t, s)` was byte-identical to
`boldColor(t, Color::None, s)` by construction — the same `attrs` check and the
same `sgrWrap(text, "1")`. Two copies of a degradation rule that REQ-UI-21/-23
make subtle is one copy too many; the rule now lives in `boldColor` alone.

**4. Removed a provably-dead de-duplication in `discoveredResistances()`.** The
loop ran `std::find` over the accumulated elements before pushing. It could never
fire: `SELECT DISTINCT detail` makes the details unique, and every detail
surviving the archetype filter shares the prefix `"<archetype>|"`, so their
element halves are unique too. Replaced with a comment stating that invariant —
the reason it is safe is worth more than the branch was.

## Deliberately not changed

- **`row.spans.back().pad = ""` in `hostileRows` and `playerRow`.** Redundant
  today (those rows use `"  "` separators, whose visible glue is empty), but it
  is a cheap guard on an invariant the glue/gap split depends on. Kept.
- **`hostileRows()` length.** It reads as a sequence of clearly-labelled row
  additions — telegraph, statuses, resistances — each tied to its requirement.
  Splitting it would scatter that mapping without reducing complexity.
- **The word-splitting loops in `term.cpp` and `band.cpp`.** Superficially
  similar, but they wrap different things under different rules: `term.cpp`
  splits prose on any whitespace and never breaks a word; `band.cpp` splits
  fields and carries per-word styling and separators. Sharing them would couple
  two units that are deliberately independent — `term.cpp` includes no db header
  and `band.cpp` is the only styling site.

## Note on line count

`band.cpp` went 519 → 524 lines. Code shrank; the comments explaining *why* the
removed branch was dead and *why* the list invariant lives where it does grew by
more. That is the intended trade in a codebase this comment-dense.
