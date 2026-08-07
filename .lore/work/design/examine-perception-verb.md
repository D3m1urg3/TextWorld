---
title: "examine — the perception verb"
date: 2026-08-06
status: approved
tags: [examine, isa, perception, canon-description, scope, narration, template-fallback]
modules: [parser, systems, render, prose, nlresolve, mutations]
related: [.lore/work/brainstorm/npcs.md, .lore/work/research/llm-npc-dialogue-and-memory.md, .lore/work/design/bard-architect-integration.md, .lore/vision.md]
---

# examine — the perception verb

The smallest of the three NPC bricks, and the only one that is not really about
NPCs. Today a materialised story character stands in a room with a working
parser noun and no way to look at it. This closes that, for every entity at
once.

## The finding that shapes everything below

**Every entity that can appear in the world already has a canon description
row.** Five writers put them there and there is no sixth:

| Writer | Line | What it stores |
|---|---|---|
| `seed/base.sql` | — | hand-authored prose for both rooms and all three items |
| `dropGrimoire` | `mutations.cpp:213` | the grimoire flavour's description |
| `placeEnemy` | `mutations.cpp:290` | the bestiary blurb as the instance's canon prose |
| `writeGeneratedRoom` | `mutations.cpp:499` | the architect's room prose |
| `placeCatalogEntry` | `mutations.cpp:707` | the room-specific prose written for a materialised story entry |

The only named thing without one is the player, and `base.sql` says that is
deliberate: *"no description: the player is not canon prose."*

So `examine` needs **no generation, no new AI role, and no write path**. It is a
`SELECT` against a table that is already full. That is the entire feature, and
it is why this brick is small enough to go first.

## What changes

### The ISA gains one verb

`Verb::Examine`, carrying `subject` = the entity id. Same shape as `Take`,
`Drop`, and `Read`.

**The fixed-verb parser gains `examine` and `x`, and nothing else.** In
particular `look` keeps its current behaviour of ignoring any trailing argument,
so `look around` still looks. "Look at the candle" is a natural phrasing, and
natural phrasings are the resolver's job — that is what it exists for. Teaching
the deterministic parser to split `look at <noun>` from `look <anything>` buys
one phrasing and risks the one verb every fallback path depends on.

Bare `examine` with no argument returns `nullopt`, like `take` and `drop`
(REQ-PROTO-6a). An argument that names nothing anywhere in the world also
returns `nullopt` — recognition only, exactly as `take` does.

### Scope: in the room, or in your hands

`resolveExamine` accepts the subject when its `location.container` is either the
player's room or the player. Refusals reuse wording that already exists:

| Case | Result |
|---|---|
| container = player's room | describe it |
| container = player | describe it |
| anything else, or no location row | `You don't see that here.` |

No new refusal strings. Note this is *wider* than `take`'s scope, which also
demands `portable` — that is the point. Enemies, story characters, and fixed
scenery are all examinable and none of them are portable.

**The player is in scope**, because the player's container is the room and no
special case excludes them. `examine player` therefore lands on the
missing-description fallback below. That is a slightly ugly sentence and a
one-line fix is available (a `description` row for entity 3 in the seed), but
`base.sql` made that omission on purpose, so this design leaves the decision
where it was rather than quietly reversing it.

### Where the text comes from

The `description` row, verbatim. When there is no row:

```
You see nothing special about the <name>.
```

Engine-authored, deterministic, and the classic answer. It exists for the player
entity and for any future entity someone forgets to give prose to — it is a
safety net, not a designed experience.

### A new event verb, not an overloaded one

`examined`, with `subject` = the entity, `object` = 0, `detail` = NULL.

The tempting alternative is `looked` with `detail = "examine"`, since
`render.cpp` already branches on `detail == "inventory"` and even leaves a
comment inviting it. **Rejected.** `mutations.hpp` warns that `events.detail`
already carries two different kinds of value — a model-facing prose fragment,
and an engine-internal tag on exactly three verbs — and the next person adding a
detail needs to know it. Adding a third meaning to that column to avoid adding a
row to a free-text verb column is the wrong trade. Verbs are cheap here.

`examined` is **not** a bard wake trigger. The bard wakes on irreversible change
(`generated`, `defeated`, `learned`, `materialized` — the four-verb predicate
from `bard-fact-store.md` decision 5, not the three the dungeon master
brainstorm listed before `materialized` existed); looking at something changes
nothing.

### It costs a turn

`look` costs a turn, and `examine` is the same kind of act — observing the
world. So enemies take their turn and the chip clock advances. Examining
something mid-fight is a real choice with a real price.

The contrast is `spells`, which is free precisely because it is reference
information about the *rules* rather than an observation of the *world*.
`examine` sits on the world side of that line.

Worth stating explicitly: examining an enemy reveals **nothing mechanical**. The
prose it prints is the bestiary blurb, which is selection text; health,
resistances, and telegraphs live in the status band and are engine-authored.
There is no way to turn `examine` into a stat sheet.

### Rendering, both paths

**Template** — `render.cpp` gains one branch: `examined` prints the description
row's prose, or the fallback line. Every other template stays byte-identical.

**AI narration** — examine rides the ordinary path. Its canon prose must appear
in the model's output word for word, the same rule that already governs room
descriptions.

Implementation detail worth fixing now rather than discovering later: the
validation gate's clauses a–e are pinned by tests, and clause c is specifically
about the *room's* canon text. Rather than widen clause c, **add clause f**:
"examined canon prose present verbatim," reading a new field on `TurnFacts`.
Clauses a–e stay untouched and so do their tests.

### The resolver needs a wider view of the room

This is the only part with any real surface area, and it is the thing the README
already identified as the blocker: *"both the narrator's facts and the
resolver's scope list only portable things."*

Three changes in `nlresolve.cpp`:

1. `examine` joins the tool's verb enum and `verbFromWord`. These two lists must
   stay element-wise identical — the suite asserts it, because a verb in one and
   not the other fails silently.
2. The scope payload gains a `things` field: the noun words of **all** named
   entities in the player's room, portable or not, ordered by entity id. `items`
   keeps its current meaning so nothing that reads it changes.
3. The system prompt's verb count and its subject rule both move: a subject may
   now come from `items`, `inventory`, **or** `things`.

One consequence to accept deliberately: with `things` on the wire, the model can
now emit `take goblin`. That is fine and needs no guard — the payload is
recognition only, and `resolveTake` already answers with `You can't take that.`
The existing rule that validity is the engine's decision covers it exactly.

## What this is not

- **No generation.** An entity without a description gets the flat line, not a
  model call. Generating descriptions for things that lack them would put a
  network round trip inside the tick transaction — the thing the architect
  accepted once and then spent an entire feature undoing — in exchange for
  nearly nothing, since the description table is already full. If it is ever
  wanted, it is its own brick with its own write path.
- **No writes** beyond the one event row.
- **No band change.** Examine adds no state to display.
- **No new AI role.** `AiRole` stays at three.

## Cost and risk

Zero added cost: no new model call, and the narrate call was happening anyway.
Zero added latency.

The risk is entirely in the resolver prompt. Widening the scope payload and
adding a twelfth verb is the kind of change that can quietly degrade the other
eleven, so the test that pins the enum against `verbFromWord` matters more than
usual, and the offline resolver tests should be re-run against the existing
phrasings rather than only the new one.

## Testing shape

Offline, no network, consistent with everything else here.

- Parser: `examine`/`x` with a known noun, an unknown noun, and bare.
- Scope: in-room, carried, elsewhere, no location row — asserting the exact
  refusal string.
- Description source: an entity with a row, and one without, asserting the
  fallback line.
- Template render: `examined` output byte-exact; every other verb's output
  unchanged.
- Narration: clause f accepted and rejected over canned responses; clauses a–e
  unchanged.
- Resolver: the enum/`verbFromWord` equality assertion, the `things` field
  present in a captured request body, and a sweep confirming no id reaches the
  wire.
- Turn cost: an `examine` with a hostile present advances the enemy's turn.

## Decision

Build it as described: one new ISA verb, one new event verb, a `SELECT` against
the existing `description` table, a flat fallback line, one new render branch,
one new validation clause, and a wider resolver scope payload. No generation, no
writes, no new AI role.

The alternative worth naming and rejecting is **generate-on-first-examine**,
which would make every entity richly described. It fails on cost and on
sequencing: it puts a model call inside the tick, adds a write path and a new
failure mode, and buys almost nothing today because the description table is
already populated for every entity the world can contain. It stays available as
a later brick if the flat fallback ever starts showing up in play — which,
given the table above, it should not.
