---
title: "Implementation plan: mage-school seed world"
date: 2026-07-09
status: executed
tags: [plan, seed, setting, worldbuilding, tests, fixture, mage-school]
modules: [seed, tests]
related: [.lore/work/specs/mage-school-seed.md, .lore/work/brainstorm/mage-school-setting.md, .lore/work/specs/engine-foundation-prototype.md, .lore/work/specs/story-seed-architect.md]
---

# Implementation plan: mage-school seed world

Implements `.lore/work/specs/mage-school-seed.md` (REQ-MAGE-1..6): replace the
Elwyn Priory default world with **Thornmere Hall, a manor–castle school of
magic at night, explored by a new student on their first night** (source:
`.lore/work/brainstorm/mage-school-setting.md`). Scope is content only —
`seed/setting.txt`, `seed/base.sql`, the test decoupling that makes the
content swap safe, and doc touch-ups. **No engine, schema, or prompt code
changes.** Gameplay (spells, NPCs) is explicitly out of scope.

## Decisions already made (do not relitigate)

- Night / new student / manor–castle mix (brainstorm).
- **Tests decouple from the shipped seed via `tests/fixture.sql`**, a
  byte-copy of the current `seed/base.sql` — user approved.
- **Priory content is replaced outright**; git history is its archive — user
  approved.
- **Wand is id 6, placed in room 1** (on the desk) — user approved; resolves
  the brainstorm's open placement question.

## Constraints the new content must respect

Live invariants from implemented specs:

- **REQ-PROTO-2** pins `base.sql`'s shape: exactly 2 rooms, one bidirectional
  exit pair from the invertible direction set, ≥2 portable items with **at
  least one in each room**, player in room 1, a `description` row for every
  room and item, **none for the player**, `meta.turn = 0`. Literal ids with
  the ledger comment convention.
- **REQ-ARCH-1**: `setting.txt` loads tolerantly into `meta.setting`; no code
  change needed. It is the architect's *only* shared context (REQ-ARCH-7a),
  so it must stay coherent with the two seeded rooms.
- **REQ-ARCH-8**: only invertible directions generate (`north↔south`,
  `east↔west`, `up↔down`, `in↔out`). Seed exits and deliberately unmapped
  "architect-bait" directions must come from this set.
- `setting.txt` feeds **only the architect** — never the prose renderer or
  resolver. The five seed description texts must carry the night-school tone
  entirely on their own (REQ-MAGE-3).
- `base.sql` runs only on a **freshly created world file**: an existing local
  `world.db` keeps the old content and must be deleted to see the new world
  (worlds are disposable this phase; no migration).
- Setting prose must not promise what the engine can't deliver — no people,
  no NPCs (the night premise exists to make that natural).

## Step sequence

<div style="font-family:monospace; line-height:1.9; border:1px solid #ccc; border-radius:6px; padding:12px 16px; overflow-x:auto;">
<b>1</b> test decoupling (fixture.sql + ~34 redirects) <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span><br>
&nbsp;&nbsp;└─ 🚦 <b>gate: suite green, zero assertion edits</b> — nothing content-side starts before this<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>2</b> setting.txt rewrite <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>3</b> base.sql rewrite <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> <i>(2 and 3 are order-independent; both feed 4)</i><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>4</b> structural test for the shipped seed <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>5</b> doc touch-ups <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> <i>(4 and 5 are order-independent; both feed 6)</i><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>6</b> deterministic validation sweep (final) <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─░ optional, user-triggered only: gated architect live smoke <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH — live LLM, not a gate</span><br>
</div>

Risk legend: <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified · <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH</span> live-LLM verification (kept outside the done-criteria).

---

## Step 1 — Decouple the test suite from the shipped seed

**Requirements:** REQ-MAGE-4. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW–MEDIUM</span> — mechanical but wide (~34 swaps); risk bounded by the zero-assertion-edit gate: if any assertion starts needing edits, stop — something's wrong.

*This lands green **before** any content changes: it proves the decoupling
independently of the reskin.*

1. Create `tests/fixture.sql` as a **byte-for-byte copy** of the current
   `seed/base.sql` (the two-room Priory world the assertions were written
   against). Use `cp`, not retyping.
2. In `tests/tests.cpp`, redirect every reference to `"seed/base.sql"` to
   `"tests/fixture.sql"` — **~34 occurrences, including the `seedPath`
   variable initializer**. The rule is mechanical, not judgment-based, with
   exactly one exception: the **architect live smoke** stays on
   `seed/base.sql` + the committed `setting.txt`, because it deliberately
   pairs the shipped seed with real shared context and its assertions are
   content-agnostic (generated ids > seed ids, reciprocal exits, start room
   with unmapped invertible directions — all of which hold for the new seed,
   which keeps ids 1–6 and the same north/south pair on rooms 1↔2; see
   Step 3).
3. `testWorld()` moves to the fixture **wholesale**: its hardcoded Priory
   shape counts (`entities == 5`, `descriptions == 4`) describe the Priory
   seed and stay correct only against the fixture. It is **not** widened to
   also cover the shipped seed — that is Step 4's separate function.
4. Leave all assertions untouched — that is the point of the fixture.

<div style="border-left:4px solid #1e7e34; background:#e6f4ea; padding:8px 12px; border-radius:0 4px 4px 0;">🚦 <b>Gate:</b> full build + complete deterministic suite green with <b>zero assertion changes</b>, and <code>grep -n '"seed/base.sql"' tests/tests.cpp</code> shows only the architect live smoke remaining. Content work does not start until this holds.</div>

## Step 2 — Rewrite `seed/setting.txt`

**Requirements:** REQ-MAGE-1, REQ-MAGE-3 (coherence half). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> — pure content, one file; the element checklist below is deterministic and no live-LLM check is required.

Replace the file with the Thornmere Hall setting. Approved draft (edit taste,
keep the structural elements):

> Thornmere Hall, a school of magic housed in a fortified manor on the edge of
> a dark mere. It was a castle once — the bones of towers and battlements
> remain — but four centuries of headmasters have domesticated it: wings added
> at odd angles, arrow slits reglazed as reading nooks, the gatehouse gone to
> a greenhouse. Staircases move when they think no one is watching, and the
> plan of the place has never been drawn twice the same way.
>
> It is deep night. The school sleeps — students behind shut dormitory doors,
> fires banked, corridors empty. The only light is candle-light, moonlight,
> and the occasional lamp that lights itself as you pass. You are a new
> student, arrived only this morning, awake and out of bed in a school you do
> not yet know.
>
> Tone: hushed, warm, quietly wondrous — a serious old fortress gone soft and
> scholarly. Magic here is ambient and domestic: self-lighting candles,
> restless books, doors with opinions. Nothing threatens; the only tension is
> that of being out of bed after curfew. Rooms are human in scale — a cell, a
> corridor, a classroom, a stair — bounded by the manor's walls and grounds
> and the dark mere beyond. No monsters, no crowds, no spectacle: the school
> is asleep, and every room should feel like it.

Required elements, whatever the final wording (REQ-MAGE-1's checklist):
school name + light cosmology (recurring proper nouns for the architect to
reuse); the moving-staircase / never-drawn-twice alibi for generated
geography; night + everyone asleep (emptiness made natural without ever
promising people or NPCs); new-student premise (arrived today, doesn't know
the building); ambient/domestic magic, wonder over danger, nothing threatens;
human-scale rooms; boundary shape (walls, grounds, the mere) as a hook for
the future scale/boundary feature.

Draft-time check (from plan review): the draft's light imagery
("candle-light, moonlight, a lamp that lights itself") is generic common
nouns. The light-cosmology element exists to give the architect **reusable
proper nouns**, so when finalizing the prose, name at least one recurring
light-related proper noun (alongside "Thornmere" and "the mere") rather than
leaving light purely generic.

Loads through the existing tolerant mechanism (REQ-ARCH-1) — no code change.

## Step 3 — Rewrite `seed/base.sql`

**Requirements:** REQ-MAGE-2, REQ-MAGE-3 (standalone-prose half). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> — same skeleton as the existing file, one file; Step 4's structural test verifies the shape mechanically.

Same structural skeleton as today (REQ-PROTO-2), new content. Approved id
ledger:

| id | entity | where |
|----|--------|-------|
| 1 | room 'dormitory cell' (start) | — |
| 2 | room 'corridor' | — |
| 3 | player | room 1, no description |
| 4 | portable 'candle' | room 1 |
| 5 | portable 'key' | room 2 |
| 6 | portable 'wand' | room 1 (on the desk) |

- Exits: `(1, 'north', 2)` and `(2, 'south', 1)` — same pair as today, so the
  architect live smoke's walk still starts from an unmapped invertible
  direction; all other directions stay unmapped so the architect generates
  rooms there on demand.
- Keep the ledger comment convention (`-- 1: room 'dormitory cell'`) and the
  existing `meta` tail (`turn = 0`, schema rows) identical in structure.
- Room 2 as corridor is deliberate architect-bait: corridor prose should
  imply continuation (doors, a stair) without mapping any of it.

Approved description drafts (edit taste; every room and portable needs one,
player none). Per REQ-MAGE-3 these must read as the night-school world **on
their own** — `setting.txt` never reaches the renderer or resolver:

- **dormitory cell**: "A narrow student's cell under a sloped ceiling: a bed
  with unfamiliar sheets, a desk, a trunk you have not finished unpacking.
  Moonlight through the single lancet window finds the door to the north,
  standing just ajar."
- **corridor**: "A long panelled corridor, doors shut on either side and the
  ceiling lost in the dark. Somewhere far off a stair creaks to itself. A lamp
  in a wall bracket kindles quietly as you approach, and the way south leads
  back to your cell."
- **candle**: "A stub of white candle in a pewter holder, burning with a
  small, patient flame that never seems to shorten it."
- **key**: "A cold iron key on a loop of faded ribbon, heavy as a promise,
  its teeth cut for a lock you have not found."
- **wand**: "A wand of pale ashwood, smooth where other hands once held it,
  left on the desk as though someone knew you were coming."

The wand is deliberately inert flavor — pure description, promising no
mechanics (REQ-MAGE-3).

Note the take-effect caveat for anyone testing by hand: the new seed appears
only in a freshly created `world.db`; delete the local one first.

## Step 4 — Structural test for the shipped seed

**Requirements:** REQ-MAGE-5. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> — one deterministic test function running plain SQL checks.

After Step 1, nothing exercises the shipped `seed/base.sql` deterministically.
Add one **new, separately named function** — `testShippedSeedShape()`, not a
widened `testWorld()` — that opens a temp world from `"seed/base.sql"` with
the default setting path and checks REQ-PROTO-2 **structurally, never
wording** (no hardcoded entity counts beyond what REQ-PROTO-2 mandates):

- exactly 2 rooms; a reciprocal exit pair whose directions are mutual
  inverses from the invertible set;
- player exists, located in room 1, has **no** description row;
- ≥2 portables, at least one located in each room;
- every room and portable has a non-empty `name` and `description`;
- `meta.turn = 0` and `meta.setting` non-empty.

This stays green across any future content swap — content changes never touch
tests again, which was the point of the fixture decision.

<div style="border-left:4px solid #1e7e34; background:#e6f4ea; padding:8px 12px; border-radius:0 4px 4px 0;">🚦 <b>Gate:</b> full suite green — fixture tests unchanged and passing, <code>testShippedSeedShape()</code> passing against the new seed.</div>

## Step 5 — Doc touch-ups

**Requirements:** REQ-MAGE-6. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> — four grep-verifiable hunks in two files.

- `README.md` ~line 44 — first-launch description: stone hall / garden /
  lantern / key → dormitory cell / corridor / candle / iron key / wand.
- `README.md` ~line 86 — shipped-setting paragraph: ruined priory → Thornmere
  Hall.
- `README.md` ~lines 19 and 60 — example phrasings mention the lantern
  (`pick up the lantern`); change to the candle so examples match the shipped
  world.
- `.lore/work/specs/story-seed-architect.md` — REQ-ARCH-1's stale
  parenthetical "(the stone hall / garden)" → the new room names, so the
  implemented spec doesn't point at content that no longer exists.

Deliberately untouched (spec's out-of-scope list): the prompt example strings
in `src/prose.cpp:150` and `src/architect.cpp:146` (generic
prompt-engineering examples, not seed references), and historical lore
documents describing the Priory (frozen record of past work).

## Step 6 — Final validation sweep (deterministic, bounded)

**Requirements:** all (spec's AI Validation, items 1–7). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> — every check is deterministic and bounded; none calls a live LLM.

Walk the spec's **AI Validation** section end to end and confirm each item:

1. **Build + suite**: full build and complete deterministic test suite pass
   (Step 1's gate already proved green-before-content).
2. **Decoupling audit**: `grep -n '"seed/base.sql"' tests/tests.cpp` returns
   only the architect live smoke and `testShippedSeedShape()`.
3. **Shape check**: `testShippedSeedShape()` passes against the new seed.
4. **Content checklist**: grep/inspect `seed/setting.txt` for each REQ-MAGE-1
   element (name, **light cosmology as its own item — not folded into
   "name"**, night, new student, alibi, tone constraints, boundary);
   inspect `seed/base.sql` for the id ledger, exits, and item placement of
   REQ-MAGE-2.
5. **Playthrough script** (fixed, non-interactive judgment): delete local
   `world.db`, run the binary, execute exactly: `look`, `take candle`,
   `take wand`, `go north`, `take key`, `inventory`, `go south`, quit,
   relaunch, `look`. Verify: cell prose on first look, corridor prose after
   `go north`, key visible in corridor, all three items in inventory, world
   resumes after relaunch.
6. **Docs audit**: `grep -in 'priory\|stone hall\|lantern' README.md` returns
   nothing (or only intentional matches).
7. **REQ-MAGE-3 tone-standalone prose**: human read-through of the five
   descriptions against the REQ-MAGE-3 criteria — deliberately **not** a
   live-LLM judgment.

<div style="border-left:4px solid #b00; background:#fce8e6; padding:8px 12px; border-radius:0 4px 4px 0;">░ <b>Optional — user-triggered only, never part of the done-criteria:</b> one gated architect live smoke (<code>TEXTWORLD_AI_LIVE_TEST=1</code>) to eyeball that generated rooms read as Thornmere Hall. Single bounded run, human reads the verdict. Per [[verification-must-be-bounded]], this must not become a tune-retry loop; the deterministic sweep above is the gate.</div>

## Out of scope

- Spell mechanics, `known_spells`, NPCs (future sessions per brainstorm).
- Prompt example strings inside `src/prose.cpp` / `src/architect.cpp` —
  generic prompt-engineering examples, not seed-coupled.
- Any schema or engine change; any world migration (worlds are disposable).
