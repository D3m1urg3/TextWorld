---
title: The story arc and the evolving setting — what the bard was supposed to be
date: 2026-09-06
status: resolved
tags: [bard, story-arc, evolving-setting, overture, faction, ledger, emergence, determinism, unfolding, catalog]
modules: [bard, architect, world, mutations]
related: [.lore/work/brainstorm/dungeon-master.md, .lore/work/design/bard-fact-store.md, .lore/work/design/story-seed-architect.md, .lore/work/brainstorm/ai-integration-points.md, .lore/work/design/bard-overture-and-scheduling.md, .lore/vision.md]
---

# The story arc and the evolving setting — what the bard was supposed to be

## Context

The README lists three missing things, one of which is *"the setting is still a
hand-written file, not something the game evolves."* This session started as a
design discussion about that, and turned into something more useful: finding out
that most of the feature was already designed in August and never built.

The session also surfaced a confusion worth recording. The bard was remembered as
the piece that owns story evolution. It does not, and the gap between what was
designed and what shipped is the reason.

## The finding: the arc was designed, then lost

`dungeon-master.md` §13 lists five things the opening call was meant to author.
`dungeon-master.md` §7 lists three kinds of fact, the middle one named
**"Arc + catalog"** — *"what could happen, and who exists in potential."*

Only the cast was built.

| Designed in the brainstorm | Built | What happened |
|---|---|---|
| Who exists, and what each wants | **yes** | the `catalog` table |
| The faction / antagonist force | no | **vanished** — zero mentions after the brainstorm |
| The clock — what is getting worse | no | killed on purpose, `bard-fact-store.md:108` |
| Puzzle kinds — lock/key pairs | no | killed on purpose, `dungeon-master.md` §8 revision |
| What would constitute an ending | no | **vanished** |
| The arc itself | no | **vanished** — the word appears zero times in any bard design, spec, plan, or note |

Two were dropped with reasons written down. Three were dropped with nothing
written down at all — they were in the brainstorm on 2026-08-03 and gone from the
first design the same day.

### Why it probably happened

`bard-fact-store.md` was deliberately scoped as storage only: *"schema, seed data,
and mutation helpers only, no AI and no network."*

When the task is a schema, the output is tables. A cast fits a table beautifully —
many rows, one shape. An arc does not. Neither does a faction, or an ending. Those
are single facts about the whole world.

They had nowhere obvious to go and fell out. The irony is that `meta.setting`,
`meta.bard_journal` and `meta.bard_focus` are all one-off `meta` rows. An arc
would have been one more row. Nobody proposed it.

### What the bard actually produces today

```
struct OvertureProposal {
    std::vector<CatalogEntryProposal> entries;
    std::string journal;
};
```

A list of nouns and a private notebook. That is the entire creative output for a
world.

And the "beats" in a live world file are not events. They are props:
`vigil lamp`, `sulking door`, `restless staircase`, `restless primer`,
`drowsy portrait`, `cold draught`. A catalog row can only hold `name`, `blurb`,
`motive`, `tier` — every field describes a **noun**. Asked for a beat, the only
thing the model could express was another object. The table shape decided the
outcome.

`motive` is decorative: one word from a fixed list of eight, feeding only the text
the model reads when choosing what to place. `appetite — wants to take and carry
off`, and nothing in the game has ever taken or carried anything off.

## The reframe that made it tractable

Stated by the user: **the setting file is the ground the AI grows a setting from,
not the setting itself. Think of a god who creates a universe and lets it
unfold.**

That moves all the stochastic work to one moment. AI creates; engine runs. It is
the cleanest possible split, drawn once in time rather than scattered through
every feature. And it makes the setting evolve without anything ever being
rewritten, which satisfies `bard-fact-store.md` Decision 1 completely instead of
working around it.

A second reframe resolved an apparent contradiction. The user wants **minimal
intervention** *and* **a more powerful bard**. Those only conflict if power means
acting often. Today's bard is the worst combination available: it acts every few
turns and can do almost nothing. The move is to make it strong and rare.

## Approaches considered

### On the setting

- **Multiple authored settings, one active.** `setting.night.txt`,
  `setting.overrun.txt`; engine picks. Nearly free. Dies on the second axis —
  night × overrun × alarm is eight hand-written files. A lookup table pretending
  to be a system. **Rejected.**
- **The gradient becomes data.** *"Deeper is more contested"* is currently a
  sentence the architect interprets. Make it a number the engine owns. Survives
  into the conclusion in weakened form (the step counter).
- **Computed present tense.** Derive a few sentences from the event log
  deterministically, no new storage. Elegant, very vision-compliant, but only
  evolves along hardcoded axes. **Partly adopted** — the ledger does this.
- **Scars.** Local permanent damage the architect must honor nearby. Not really
  the setting evolving; the world accumulating damage. **Parked, still good.**
- **The addendum lane, revived.** Rejected again, same reason as
  `bard-fact-store.md` Decision 1, plus: unbounded growth, and the obvious fix is
  a summarizer, and a summarizer compressing world-facts is rewriting permanent
  content through the back door.

### On mechanics

- **The school waking up.** Noise wakes the school. Waking is good (characters
  appear, they help) and bad (you are a first-night student out of bed). A real
  trade-off with no dice. Also a possible answer to the README's *"major
  characters have no way into the world"* — waking is the arrival. **Parked, and
  the strongest of the unused ideas.**
- **Pressure as a front you push.** Clearing lowers it locally, it regrows from
  the breach. Risk: quietly becomes a strategy game, a different genre than the
  vision describes.
- **The symmetric race.** Every grimoire you take, they take one. Progression and
  threat on the same axis, no timer anywhere. **Adopted in spirit** — the steps
  advance on player progress.
- **Night phases as a progress bar, not a doom clock.** Dawn as the win condition
  creeping closer rather than a deadline. **Parked** — a good second list of steps
  once one list works.

### On regions (parked, probably needed later)

A world cannot unfold into space that does not exist, and lazy generation is what
makes this project affordable. The way out: creation writes **regions**, not
rooms — a dozen named areas with relationships. Laws run on regions; rooms
materialize inside them and inherit the region's state at build time.

This is the LOD ladder from `story-seed-and-lod-world.md` one level up, and it
makes offscreen change nearly free — a dozen regions, not a world. It may also
quietly answer the L3 Alive question deferred twice.

**Not in v1.** The step counter delivers the felt result without geography,
because the bard writes the prose of each step and the geography lives in that
prose.

## The conclusion

**The bard writes the whole story once, at world creation. The engine runs it.
Nobody intervenes after that.**

Three parts:

1. **The arc** — one `meta` row, written by the opening call, never changed. What
   the story is about, what the enemy force wants, what ending would settle it.
   This is the piece that vanished.
2. **The steps** — a short ordered list of how the threat gets closer, five or six
   entries, each a sentence of prose. Written once, with the whole picture in
   view.
3. **The ledger** — the engine advances the steps and writes each advance to
   `events`, alongside `moved` and `defeated`. That table is already append-only.
   It has no story verbs in it yet.

### Why it does not break what was already decided

| Rule | How it holds |
|---|---|
| No clock (`bard-fact-store.md:108`) | Steps advance on irreversible player events — `generated`, `defeated`, `learned` — never on turns. Idle at the safe edge and nothing moves. The player is the clock. |
| No command channel (`dungeon-master.md` §10) | The bard writes what exists and what is true. The engine advances. The architect reads. Nothing is ever told to act. |
| Rooms are never rewritten | Old rooms do not change. New rooms are built knowing the current step, so they are born into a worse world. |
| The setting file is immutable | Never touched. |
| Engine owns every number | The bard writes prose per step; the engine owns which step is current and what advances it. |

### What it is not

One list of steps is a fuse, not a simulation. The order is authored, so the
sequence is not a surprise.

Emergence comes from several lists running at once — the enemy closing on what it
wants, the school waking, a character's secret coming apart — each advancing on
different actions, producing combinations nobody wrote. That is the same code with
more rows, so it is a growth path rather than a rewrite. **But v1 with one list
will feel more like a fuse than a world.** Recorded now rather than discovered
later.

### Build order

The bard's own pattern: storage, then the call, then the consumers.

1. **Storage and the advance rule.** Arc row, steps table, the advance rule, the
   new event verbs. No AI, no network. Hand-write a list of steps in the seed and
   test the whole thing with SQL. **All the risk is here, and it needs no API key.**
2. **The opening call writes them.** Extend the existing overture. One call at
   creation, checked at admission the way catalog entries already are.
3. **Consumers.** The architect sees the current step when building a room. The
   narrator can say when something advanced. The bard sees its own history on wake.

## Decided (2026-09-06)

**1. The bard wakes only when a step advances.** It stops waking on generic
engine events. Today's trigger — `generated`, `defeated`, `learned`,
`materialized`, at most once every 5 turns — is replaced by a single condition:
a step moved. That is roughly five wakes in a playthrough instead of dozens.

Two things follow. It is **self-limiting**: a bard that authored no steps never
wakes again, so the cost of a quiet story is zero. And every wake is *consequence*
rather than noise — the bard is asked "what now" only when something it set up has
actually happened, which is also the only moment it has anything new to say.

Rejected: going fully quiet after creation. Purer and cheaper, but a thin opening
call would ruin a world with no way to recover, and you would not find out for
hours. Keeping a rare wake is the recovery path.

*Consequence for the build:* `hasTriggeringEvent` in `bard.cpp:1089` gets
**simpler**, not more complex — one verb instead of four. `kBardMinTurnGap` may
become unnecessary, since the advance rule already bounds the rate.

**2. Each step names a condition; the engine owns the vocabulary.** The engine
ships a closed set of conditions it knows how to check. The bard picks one per
step. The engine evaluates them and advances.

```
  n  after                 prose
  1  enemies_defeated:2    "They have breached the lower stair."
  2  spell_learned:frost   "Something is moving in the outer stacks."
  3  rooms_built:6         "They have found the index."
```

This is the pattern the project already trusts three times over — `bestiary`
archetypes, `eligibleArchetypes`, the catalog itself. The model selects; the
engine owns every number and every rule. It also keeps brick 1 free of AI: the
conditions are hand-written in the seed and the whole advance rule is testable
with SQL.

Rejected: any irreversible event advancing by one. Trivial to build, but the story
would advance at the same rate no matter what you did, which is the opposite of
the point. Also rejected: per-step costs against a running total — more pacing
control than the flat rule, but the weights are numbers somebody has to pick, and
the condition vocabulary gives better control for the same work.

*Open for the spec, not for this brainstorm:* what is actually in the condition
vocabulary. It needs to be small, checkable with one query each, and expressible
in the events table as it already stands.

## Still open, not part of this feature

- **What ends the game.** `ai-integration-points.md` asked *"win condition vs
  endless"* in April and nothing has had a home for it since. The arc is the first
  thing that does — it can name an ending. Whether the engine can *detect* one is
  separate and unanswered.
- **Do characters act on their own?** Whether the scholar actually moves and hides
  something, or whether motive only shows up in what she says when you meet her.
  Very different amounts of work; the second is nearly free with what exists.
- **The `sketch` lane / L1.** Deferred three times now. Still shape-compatible.
