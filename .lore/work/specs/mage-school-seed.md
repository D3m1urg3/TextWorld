---
title: Mage-school seed world (Thornmere Hall)
date: 2026-07-09
status: draft
tags: [seed, setting, worldbuilding, tests, fixture, mage-school]
modules: [seed, tests]
related: [.lore/work/brainstorm/mage-school-setting.md, .lore/work/specs/engine-foundation-prototype.md, .lore/work/specs/story-seed-architect.md]
req-prefix: MAGE
---

# Mage-school seed world (Thornmere Hall)

Replace the default Elwyn Priory world with a mage-school setting: **Thornmere
Hall, a manor–castle school of magic at night, explored by a new student on
their first night.** Source decisions: `.lore/work/brainstorm/mage-school-setting.md`.

Content-only change: `seed/setting.txt`, `seed/base.sql`, the test decoupling
that makes content swaps safe, and doc touch-ups. **No engine, schema, or
prompt code changes.** Spells, NPCs, and all gameplay mechanics are out of
scope (future sessions).

User-approved decisions this spec encodes: tests decouple from the shipped
seed via a fixture copy; the Priory content is replaced outright (git history
is its archive).

## Requirements

### REQ-MAGE-1 — Setting document replaced

`seed/setting.txt` is replaced with the Thornmere Hall setting. Whatever the
final prose, it must contain all of:

- a school **name and light cosmology** (recurring proper nouns for the
  architect to reuse);
- the **night premise**: the school asleep, corridors empty — emptiness made
  natural without ever promising people or NPCs;
- the **new-student premise** (arrived today, doesn't know the building);
- the **shifting-geography alibi** (moving staircases / plan never drawn the
  same twice) so architect-generated layouts are lore-accurate;
- tone constraints: magic is **ambient and domestic**, wonder over danger,
  nothing threatens, rooms human-scale;
- **boundary shape** (walls, grounds, the mere) as a hook for the future
  scale/boundary feature.

It loads through the existing tolerant mechanism (REQ-ARCH-1); no code change.

### REQ-MAGE-2 — Seed world replaced, shape preserved

`seed/base.sql` is replaced with the mage-school world while conforming
exactly to the shape pinned by REQ-PROTO-2: 2 rooms, one bidirectional exit
pair from the invertible direction set, ≥2 portables with at least one in each
room, player in room 1 with no description row, a name and description row for
every room and portable, `meta.turn = 0`, literal ids with the ledger comment
convention.

Content and id ledger: 1 = room **dormitory cell** (start), 2 = room
**corridor**, 3 = player, 4 = portable **candle** (room 1), 5 = portable
**key** (room 2), 6 = portable **wand** (room 1). Exits `(1,'north',2)` /
`(2,'south',1)`; all other directions stay unmapped so the architect
generates rooms there on demand.

The seed takes effect only on a **freshly created world file**: an existing
local `world.db` keeps its old content and must be deleted to see the new
world (worlds are disposable this phase; no migration).

### REQ-MAGE-3 — Prose carries the tone standalone

`setting.txt` feeds only the architect, never the prose renderer or resolver —
so the five seed description texts (2 rooms + 3 portables; the player has
none) must read as the night-school world on their own. The wand's description is pure flavor and must promise no mechanics.
Setting and room prose must stay mutually coherent (the setting is the
architect's only shared context, REQ-ARCH-7a).

### REQ-MAGE-4 — Tests decoupled from shipped seed content

A new `tests/fixture.sql` preserves the current Priory two-room world
byte-for-byte as the test fixture. The rule is mechanical, not judgment-based:
**every reference to `"seed/base.sql"` in `tests/tests.cpp` (~34 occurrences,
including the `seedPath` variable initializer) is redirected to the fixture,
except the architect live smoke** — with **zero assertion changes**. In
particular `testWorld()` moves wholesale: its hardcoded shape counts
(`entities == 5`, `descriptions == 4`) describe the Priory seed and stay
correct only against the fixture. The architect live smoke stays on
`seed/base.sql` + committed setting (its assertions are content-agnostic and
must hold against the new seed: generated ids > seed ids, reciprocal exits,
start room with unmapped invertible directions).

### REQ-MAGE-5 — Shipped seed gains a structural test

One new deterministic test — a **new, separately named function, not a
widened `testWorld()`** — opens a temp world from `seed/base.sql` with the
default setting path and verifies the REQ-PROTO-2 shape plus non-empty
`meta.setting` — structure only, never wording (no hardcoded entity counts
beyond what REQ-PROTO-2 mandates) — so the shipped seed stays exercised after
decoupling and future content swaps never touch tests.

### REQ-MAGE-6 — Documentation updated

`README.md` no longer describes the Priory: the first-launch description
(line ~44), the shipped-setting paragraph (line ~86), and the example
phrasings mentioning the lantern (lines ~19, ~60) reflect the new world. The
stale parenthetical "(the stone hall / garden)" in
`.lore/work/specs/story-seed-architect.md` (REQ-ARCH-1) is updated to the new
room names.

## Complexity and token-risk assessment

Per standing practice: size by shape, deterministic verification = low risk.

| Requirement | Size | Token risk | Why |
|-------------|------|-----------|-----|
| REQ-MAGE-1 setting.txt | S (1 file, prose) | **Low** | Pure content; element checklist is deterministic; no live-LLM check required |
| REQ-MAGE-2 base.sql | S (1 file, SQL) | **Low** | Same skeleton as existing file; structural test verifies |
| REQ-MAGE-3 tone-standalone prose | S (inside 1+2) | **Low** | Human-review criterion at spec/plan review time, not a live-LLM loop |
| REQ-MAGE-4 fixture decoupling | M (~34 mechanical string swaps + 1 new file) | **Low–medium** | Mechanical but wide; risk bounded by "zero assertion changes" gate — if assertions start needing edits, stop, something's wrong |
| REQ-MAGE-5 structural test | S (1 test fn) | **Low** | Deterministic SQL checks |
| REQ-MAGE-6 docs | S (4 hunks) | **Low** | Grep-verifiable |

**Whole-feature risk: low.** No step requires live-LLM verification. The one
optional live check (architect smoke against the new setting) is user-triggered,
gated, and single-run — never part of the done-criteria.

## AI Validation

All checks are deterministic and bounded; none call a live LLM.

1. **Build + suite**: full build and the complete deterministic test suite
   pass after each requirement lands (REQ-MAGE-4's gate: green with zero
   assertion edits, *before* any content changes).
2. **Decoupling audit**: `grep -n '"seed/base.sql"' tests/tests.cpp` returns
   only the architect live smoke and the new structural test.
3. **Shape check**: the REQ-MAGE-5 test passes against the new seed.
4. **Content checklist**: grep/inspect `seed/setting.txt` for each REQ-MAGE-1
   element (name, night, new student, alibi, tone constraints, boundary);
   inspect `seed/base.sql` for the id ledger, exits, and item placement of
   REQ-MAGE-2.
5. **Playthrough script** (fixed, non-interactive judgment): delete local
   `world.db`, run the binary, execute exactly: `look`, `take candle`,
   `take wand`, `go north`, `take key`, `inventory`, `go south`, quit,
   relaunch, `look`. Verify: cell prose on first look, corridor prose after
   `go north`, key visible in corridor, all three items in inventory,
   world resumes after relaunch.
6. **Docs audit**: `grep -in 'priory\|stone hall\|lantern' README.md` returns
   nothing (or only intentional matches).
7. **REQ-MAGE-3 (tone-standalone prose)**: no mechanical check — verified by
   a human read-through of the five descriptions against the REQ-MAGE-3
   criteria during plan/implementation review. Deliberately not a live-LLM
   judgment.

## Explicitly out of scope (deliberate, not gaps)

- **Prompt example strings** in `src/prose.cpp:150` ("You lift the lantern…")
  and `src/architect.cpp:146` (`like "stone hall" or "chapter house"`): these
  are generic prompt-engineering examples sent to the model, not references to
  seed entities; they work regardless of world content and stay unchanged.
- **Historical lore documents** (`.lore/work/notes/story-seed-architect.md`,
  `.lore/work/design/engine-foundation.md`, implementation notes) that
  describe the Priory world: frozen historical record of past work, not live
  documentation — intentionally untouched. Only the live spec parenthetical
  named in REQ-MAGE-6 is updated.
- Spells, `known_spells`, NPCs, and all gameplay mechanics (future sessions).
