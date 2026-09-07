---
title: "examine — the perception verb: requirements"
date: 2026-08-06
status: implemented
tags: [examine, isa, perception, canon-description, scope, render, narration, resolver, requirements]
modules: [parser, systems, render, prose, nlresolve, action]
related: [.lore/work/design/examine-perception-verb.md, .lore/work/brainstorm/npcs.md, .lore/work/specs/bard-architect-integration.md, .lore/work/specs/ai-resolver.md, .lore/vision.md]
req-prefix: EXAMINE
---

# examine — the perception verb: requirements

## Context

A materialised story entity currently stands in a room with a working parser
noun and no way to look at it. The README states this plainly: *"the verb set
has no `examine`, and both the narrator's facts and the resolver's scope list
only portable things, so a story entity is present and inert."*

This spec closes that, for every entity at once. Established in
[the design](../design/examine-perception-verb.md).

**The controlling finding.** Every entity that can appear in the world already
has a canon `description` row, written by one of five places: `seed/base.sql`,
`dropGrimoire` (`mutations.cpp:213`), `placeEnemy` (`mutations.cpp:290`),
`writeGeneratedRoom` (`mutations.cpp:499`), and `placeCatalogEntry`
(`mutations.cpp:707`). The only named entity without one is the player, and
`base.sql` says that omission is deliberate.

So `examine` is a `SELECT` against a table that is already full. **No
generation, no new AI role, no write path beyond one event row, and no
`SCHEMA_VERSION` bump.**

**Scope boundary.** This spec is independent of NPCs — it is a prerequisite for
them, not part of them. Conversation, memory, profiles, and character movement
are out of scope and specified elsewhere.

## Requirements

### The ISA

**REQ-EXAMINE-1** — `Verb` gains exactly one member, `Examine`, carrying
`subject` = the entity id. No other field of `Action` is used by it. The verb
set becomes twelve.

**REQ-EXAMINE-2** — `Examine` reaches `resolve` like `Take`, `Drop`, and `Read`:
inside the tick transaction, after `meta.turn` has been incremented. It is
neither handled pre-transaction (the `Quit` / `Spells` treatment) nor exempt
from the turn.

### The deterministic parser

**REQ-EXAMINE-3** — The fixed-verb parser recognises exactly two new verb words,
`examine` and `x`, each taking a noun argument. It recognises no others.

**REQ-EXAMINE-4** — `look` behaviour is **unchanged**. It continues to ignore
any trailing argument, so `look around` still looks and `look at the candle`
still produces a bare `Look`. Natural phrasings that mean examination are the
resolver's responsibility (REQ-EXAMINE-18), never the deterministic parser's.

**REQ-EXAMINE-5** — Bare `examine` or `x` with no argument returns `nullopt`,
the same treatment `take`, `drop`, `read`, `go`, and `cast` already give a bare
verb (REQ-PROTO-6a).

**REQ-EXAMINE-6** — An argument that names no entity anywhere in the world
returns `nullopt`. Recognition uses the shared `lookupNoun`, so the parser and
the resolver's validation gate resolve a noun by the same first-match rule
(REQ-RESOLVE-14). Recognition only: whether the entity is in scope is
resolution's decision, not the parser's.

### Scope

**REQ-EXAMINE-7** — `resolveExamine` accepts the subject when its
`location.container` is either the player's current room or the player entity,
**or when the subject IS the player's current room**. It does **not**
additionally require `portable` — that is the difference from `take`, and it is
what makes enemies, characters, and fixed scenery examinable.

*Amended 2026-09-07 by [terminal-visual-polish](.lore/work/specs/terminal-visual-polish.md),
REQ-POLISH-14.* The room clause is new. A room has no `location` row, so
`containerOf` returns `nullopt` for it and `x cell` answered
`You don't see that here.` — which was fine while `look` reread the room every
time, and is not fine now that REQ-POLISH-17 makes a repeat `look` print the
room's name alone. `examine <room>` is the full reread, so the room must be in
scope. The room case is checked BEFORE the `containerOf` test, which cannot
answer for an entity with no `location` row. A room the player is NOT in stays
out of scope and still answers `You don't see that here.`

**REQ-EXAMINE-8** — A subject that is neither in the room nor carried, or that
has no `location` row at all, produces a `failed` event whose detail is exactly
`You don't see that here.` — the string `resolveTake` already uses. No new
refusal string is introduced by this spec.

**REQ-EXAMINE-9** — The player entity is in scope, because its container is the
room and no special case excludes it. `examine player` therefore reaches
REQ-EXAMINE-11's fallback. This is a stated consequence rather than a designed
experience; `base.sql`'s deliberate omission of a player description is left
undisturbed.

### The text

**REQ-EXAMINE-10** — When the subject has a `description` row, the examine
output is that row's `prose`, **verbatim**. Nothing wraps, trims, or rewords it.

**REQ-EXAMINE-11** — When the subject has no `description` row, the output is
exactly:

```
You see nothing special about the <name>.
```

where `<name>` is the subject's `name` row value. This is engine-authored and
deterministic — a safety net for entities without prose, not a generated
description.

**REQ-EXAMINE-12** — No model call is made to produce or supply examine text,
on any path, including the no-description path. `AiRole` gains no member and
stays at three.

### The event

**REQ-EXAMINE-13** — A successful examine appends exactly one event: verb
`examined`, `subject` = the entity, `object` = 0, `detail` = NULL. No component
row is written, so `examined` joins `looked`, `waited`, and `failed` as a
legal no-write verb, and `mutations.hpp`'s contract comment naming that set is
updated to say so.

**REQ-EXAMINE-14** — `examined` is **not** added to the bard's wake predicate.
The bard continues to wake only on `generated`, `defeated`, `learned`, and
`materialized`. Observing something changes nothing and is not irreversible.

**REQ-EXAMINE-15** — `looked` is not overloaded to carry examination. No new
`detail` value is introduced on `looked`, and `events.detail` gains no third
kind of meaning beyond the two `mutations.hpp` already documents.

### Turn cost

**REQ-EXAMINE-16** — An examine consumes a turn: `meta.turn` advances, and every
hostile in the room takes its turn inside the same transaction, exactly as it
does for `look`. Examining during a fight costs a tick of the chip clock.

**REQ-EXAMINE-17** — Examine emits the `description` row and nothing else. It
reads no `health`, `hostile`, `resistance`, `cooldowns`, `pending_strike`,
`status_effects`, or `barrier` row, and appends nothing derived from one. What
the hand-authored blurb itself says is seed content and outside this spec's
control; what the *engine* adds to it is nothing. Mechanical state stays the
status band's business.

### The resolver

**REQ-EXAMINE-18** — `examine` joins both the `emit_action` tool's verb enum and
`verbFromWord`. The two lists stay element-wise identical, and the existing test
asserting that equality is extended rather than replaced.

**REQ-EXAMINE-19** — The resolver's scope payload gains one field, `things`: the
noun words of **all** named entities in the player's current room, portable or
not, ordered by entity id. The existing `items` field keeps its current meaning
and contents, so nothing that reads it changes behaviour.

**REQ-EXAMINE-20** — The resolver system prompt is updated so that (a) its stated
verb count matches the twelve-verb set, and (b) a `subject` may be drawn from
`items`, `inventory`, **or** `things`. The instruction that no noun absent from
the scope facts may be introduced is unchanged in force.

**REQ-EXAMINE-21** — With `things` on the wire the model may now emit a verb
whose subject is a non-portable entity — for example `take` on an enemy. No
guard is added. The payload is recognition only; `resolveTake` already answers
`You can't take that.`, and the engine remains the sole authority on whether an
action applies (REQ-RESOLVE-14).

**REQ-EXAMINE-22** — No entity id, catalog id, tier, or engine-internal tag
enters the `things` field or any other part of the request body. Nouns only.

### Rendering

**REQ-EXAMINE-23** — `render.cpp` gains exactly one branch, for `examined`,
emitting REQ-EXAMINE-10's prose or REQ-EXAMINE-11's fallback line followed by a
newline. Every other verb's template output is byte-identical to before this
spec.

**REQ-EXAMINE-24** — On the AI narration path an examine turn is narrated
normally, and the examined entity's canon prose must appear in the model's
output **verbatim**, the same obligation room canon already carries.

**REQ-EXAMINE-25** — That obligation is enforced as a **new clause f** on the
validation gate, reading a new field on `TurnFacts`. Clauses a through e are
unchanged in behaviour, ordering, and diagnostic wording, and their existing
tests pass unmodified.

**REQ-EXAMINE-25a** — When the examined entity has **no `description` row**,
clause f is **inactive**: the new `TurnFacts` field is empty, nothing is
required verbatim, and the facts payload carries the entity's name only. The
REQ-EXAMINE-11 fallback line is a *template* string, not canon, so requiring the
model to reproduce it would pin AI output to template wording — the opposite of
what every other verb does. An empty required-text field must therefore mean
"clause f does not apply", never "the empty string was not found".

**REQ-EXAMINE-26** — A clause-f failure falls back to the template renderer for
that turn, like every other gate failure. An examine turn never fails to produce
output.

### Non-goals

**REQ-EXAMINE-27** — No description is generated for an entity that lacks one,
on first examine or ever. Generating entity descriptions is a separate feature
with its own write path, and this spec neither builds nor prepares for it.

**REQ-EXAMINE-28** — No write occurs beyond the single `examined` event row. No
component table is touched, no `description` row is created or altered, and no
`SCHEMA_VERSION` bump is required.

**REQ-EXAMINE-29** — The status band is unchanged. Examine adds no state, so it
adds no row and no field to the band's composition.

## AI Validation

All validation is offline: no network, no `ANTHROPIC_API_KEY`, fake transports
where a model is involved — the standard the existing suite already meets.

### Parser

1. `examine candle` and `x candle` in the dormitory cell both yield
   `Action{Verb::Examine, subject = 4}`.
2. `examine` and `x` alone yield `nullopt`.
3. `examine gryphon` (no such noun anywhere) yields `nullopt`.
4. **Regression:** `look`, `look around`, and `look at the candle` all yield a
   bare `Action{Verb::Look}` with no subject — asserting REQ-EXAMINE-4 by
   showing `look` is untouched.

### Scope and text

5. Examine an item in the room → its seeded description prose, byte-exact.
6. Examine an item after taking it (container = player) → the same prose.
7. Examine an item left in another room → the `failed` event carries exactly
   `You don't see that here.`, **and `meta.turn` advanced by one** — a refused
   examine is still a turn the world understood (REQ-EXAMINE-2).
8. Examine an entity with a `name` row and no `description` row → exactly
   `You see nothing special about the <name>.`
8a. `examine player` resolves (the player is in scope, REQ-EXAMINE-9) and
   produces exactly `You see nothing special about the player.` — named
   explicitly, because REQ-EXAMINE-9 is a consequence nobody would think to test
   for.
9. Examine the seeded goblin → the output is **byte-equal to its `description`
   row and contains nothing else**. Asserting equality rather than hunting for
   digits is what actually proves REQ-EXAMINE-17: nothing derived from `health`,
   `hostile`, `resistance`, or any status table can be present if the output is
   the description row exactly.
10. **Coverage guard:** for a world containing one of each — room, item,
    grimoire, enemy, materialised catalog entry — assert every one has a
    `description` row. This is the spec's controlling finding, and it should
    fail loudly if a sixth writer ever mints an entity without prose.

### Event and turn

11. A successful examine appends exactly one event row, with verb `examined`,
    the right subject, `object = 0`, and NULL detail.
12. No component table row count changes across an examine turn.
13. `meta.turn` advances by exactly one.
14. With a hostile present, an examine turn produces the enemy's chip event in
    the same transaction — the enemy acted.
15. The bard's wake predicate returns false for a turn whose only event is
    `examined`.

### Rendering

16. Template path: `examined` output is byte-exact for both the prose and
    fallback cases.
17. **Regression:** a scripted session exercising every other verb produces
    byte-identical output to a run of the same script before this change.
18. AI path over canned responses: a response containing the canon prose
    verbatim is accepted; one that paraphrases it fails clause f; the
    diagnostic names clause f.
18a. Examining an entity with **no** `description` row: any well-formed
    response is accepted, and clause f never fires. Drive it with a response
    that contains none of the template wording, proving the empty required-text
    field means "inactive" and not "not found" (REQ-EXAMINE-25a).
19. Clauses a–e reject and accept exactly as before, asserted with the existing
    canned responses unmodified.
20. A clause-f rejection produces the template output for that turn, and the
    turn still prints.

### Resolver

21. The verb enum in the tool schema and `verbFromWord` are element-wise equal —
    the existing equality test, extended to twelve.
22. A captured request body for a room containing the goblin has `things`
    including `goblin grunt` and `items` unchanged from what it was before this
    spec.
23. A body sweep asserts no integer id, `tier`, `seeded` flag, or internal tag
    appears anywhere in the request.
24. **Regression:** the existing resolver phrasing tests for all eleven prior
    verbs pass unmodified, run against a room that now also supplies `things`.

### Whole-feature gates

25. `grep -En "INSERT|UPDATE|DELETE" ` over any new translation unit this
    feature adds is empty — examine writes only through `appendEvent`.
26. The suite passes with no network and no API key.
27. `SCHEMA_VERSION` is unchanged, and a world file created before this change
    opens and plays without complaint.

## Out of scope

- Generating descriptions for entities that lack them.
- ~~`examine` on a room by name.~~ **Brought INTO scope 2026-09-07 by
  REQ-POLISH-14** — see the amendment on REQ-EXAMINE-7. `look` no longer
  describes the room every time, so `examine <room>` is now the full reread.
  Examining a room the player is not in remains out of scope.
- Any NPC behaviour: conversation, memory, profiles, movement.
- A player self-description row, which REQ-EXAMINE-9 deliberately leaves absent.
