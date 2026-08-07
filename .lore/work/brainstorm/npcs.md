---
title: NPCs — minor ones from the catalog, major ones who arrive
date: 2026-08-06
status: resolved
tags: [npc, dialogue, memory, major-npc, identity-profile, catalog, spawning, cost, latency]
modules: [bard, architect, loop, mutations]
related: [.lore/work/brainstorm/dungeon-master.md, .lore/work/brainstorm/ai-integration-points.md, .lore/work/design/bard-fact-store.md, .lore/work/design/bard-catalog-selection.md, .lore/work/research/similar-projects-and-approaches.md, .lore/vision.md]
---

# NPCs — minor ones from the catalog, major ones who arrive

## Where this starts

NPCs were parked twice: once at the very beginning, and again at the end of the
dungeon master session, which listed four open problems and moved on. This
session picks those up.

The important thing that changed in between: **NPCs are already half built.**
The story catalog has `kind = 'character'`. When the world generator writes a
room it may place one, and it becomes a real entity with a real name the parser
recognises. So a character existing in the world already ships. It just does
nothing — there is no `examine`, no way to talk to it, and it never moves.

## "NPCs" is three features, not one

| | What it adds | Size |
|---|---|---|
| Presence | you can look at one — needs `examine` | small; already flagged as the next piece |
| Voice | you can talk to one and it answers | every hard problem lives here |
| Life | it moves and acts on its own | new verbs, and it breaks one-action-per-turn |

**Build voice before life.** A character that talks but never moves is far
smaller than one that moves but never talks, and talking is the part people
want. While an NPC only talks, the player is still the only thing changing the
world.

This inverts *"NPCs are players"* — the sharpest idea from the first brainstorm.
Stated so it can be argued with later: the catalog's propose-and-check pattern
may have quietly superseded it.

## Minor NPCs

They come from the bard's catalog, exactly as they do today. The bard writes
them; the generator places one when it builds a room, coherent with the setting
and with that room. Nothing changes structurally.

Two consequences:

- The catalog has to keep growing or the cast runs dry after twenty rooms. The
  bard appends on each wake, so the cast is written a few at a time rather than
  all at once.
- They need memory, just much less of it. A character that forgets you between
  visits feels worse than one that never spoke.

A minor NPC's whole identity is a name, a line of description, the room, and the
setting. That is probably enough — a dozing porter at two in the morning writes
itself.

## Major NPCs

One to three of them. Hand-written profiles rather than model-authored, read at
world creation and handed to the bard alongside `setting.txt`, so the story it
writes accounts for them. That makes the most important characters the author's
rather than the model's, and lets the same cast be replayed in a different
world.

### They do not spawn. They arrive.

Spawning means appearing in a room. That needs machinery the engine does not
have — nothing can put a thing into the room the player is standing in — and it
always reads as a summon.

Arriving means placed once, quietly, in a room the player is not in. After that
the character exists permanently and **walks**. Every appearance is movement.

What this buys:

- One placement per character, and it is invisible, so "put a thing in the
  player's room" is never needed.
- Every later appearance uses movement, which the engine nearly does already.
- It stays honest to the rule that tension here is about places, not timers. The
  rival is not summoned by drama. It is somewhere, and it is coming.

### When

The bard decides, on one of its ordinary wakes. It already wakes on permanent
change, already reads what the player has done, and already picks from its own
catalog. "Now is the moment" is that kind of decision.

**No separate condition language.** Two systems making story calls is worse than
one, and the engine's version could only check things it can count.

The one plain limit worth keeping is depth, because it already exists: catalog
entries carry a tier and are only offered once the player is that far from the
start. A major character gets a tier meaning *not before here*, and the bard
chooses whether and when after that.

### Where

Rules the engine applies with no AI: a room that already exists, not the
player's, within a few steps. Far enough not to be seen appearing, close enough
to actually be met. A major character placed at the far edge of the map is the
boring failure.

## Files versus database

The project already has the rule. `seed/setting.txt` is the precedent: written
by hand, loaded into the world once at creation, and the world owns it after
that.

**Identity in files. Memory in the database.** Reasons memory cannot be files,
strongest first:

1. Copying `world.db` forks a world and deleting it resets one. File-based
   memory gives a copied world a character who remembers the other one, and a
   deleted world a character who remembers a game that no longer exists.
2. A turn is one transaction — all of it happens or none of it does. A file
   write cannot be rolled back with the turn.
3. Memory has to be queryable.

Memory reuses the bard's shape: a short summary the character rewrites, plus the
raw entries since it last thought. Cost stays flat — a 500-turn game costs the
same per conversation as a 50-turn one. Minor NPCs use the same table with a
smaller budget: one system, two sizes.

A rewritable summary means a character can misremember. That is fine here, and
arguably good. It is only dangerous when the thing rewriting its own past is the
story author.

## Rules that fell out

**NPCs never explain game rules.** No weaknesses, damage, cooldowns. The
knowledge-beat mechanism already exists and is checked against the real
resistance table before admission; free prose cannot be checked that way. Keeping
mechanics out of dialogue means everything an NPC says is safe to be wrong
about — which is exactly what lets it lie, be mistaken, and feel like a person.

**Cast list yes, pressures no.** This answers carried-forward question 2 from the
dungeon master session. An NPC reading the cast list can mention other characters
coherently. An NPC reading what is about to happen is the bard's mouthpiece with
a voice, which is the railroading the vision rules out.

**No NPC picks up a grimoire.** Grimoires are how spells are learned. A character
walking off with one could quietly make the game unwinnable. Cheap to enforce,
worth doing before it bites.

**Friendly only, for now.** Combat is a deterministic puzzle tuned around exactly
one player action per turn. An ally who also attacks changes all of that
arithmetic. An ally who fights is a combat rebalance, not an NPC feature.

**They do not enter rooms with hostiles.** Plain rule, no AI.

## Anything said is words until the bard promotes it

The hardest question was whether dialogue is canon. If a character says *"my
brother died in the flood,"* is there now a brother and a flood?

- Store nothing — the character contradicts itself next conversation. That is
  the slop failure this project exists to avoid.
- Store everything — the world fills with claims the engine cannot arbitrate.
- **Only the bard can make it true.** The words go in the character's memory. On
  a later wake the bard may add the brother to the catalog, or ignore it. If it
  adds him, he can eventually materialise through the ordinary path.

The third is the working answer, and it does something the dungeon master
session left open: the catalog *must* be able to grow, but nothing said where new
entries come from. This says they come from what the player and the characters
talked about.

Weakest joint: that is model proposing to model, with no engine in between,
which is not the house pattern. The defence is that materialisation still runs
the full check, so the hop only changes what is *offerable*, never what is
mechanically true — the same grant the bard already holds. Attack this first.

## Cheap autonomy

Autonomy is a feeling and most of it can be faked. A character only needs to
seem autonomous at the moment it is being looked at. Between visits it can be
frozen; when the player walks in after fifty turns, the game decides *then* what
it has been doing. One call, not fifty.

The catch: if it has been doing things, those things must be true in the
database, or the world starts lying. So offscreen life is picked from a list the
engine builds — moved next door, picked something up, stayed put — the same
propose-and-check shape as enemies and story entries. The model picks, the engine
applies, it lands in the event log, nothing was invented.

For major NPCs that act every turn, the cost is not money, it is waiting. A
second call inside the turn takes it from four seconds to eight — the problem
already solved once for room generation. The fix: **think in the background,
act at the start of the next turn.** Always one turn behind, unnoticeable, and
the machinery exists (the bard's worker thread). And when the character is not
in the player's room it needs no call at all — it moves by a plain rule.

Note: the engine foundation recorded a revisit trigger, *"when NPC systems chain
within a tick."* This is that. Deciding in the background and applying at the
start of the next turn steps around it.

## The profile experiment

A full profile was written for a Thornmere character — a fifth-year student, out
of bed the same night as the player, who wants to reach the deep stacks first.
Sections: who she is, how she talks, what she wants, what she knows, what she
does not know, what she will not say, how she behaves, and what she must never
do. About seven hundred words.

The premise held: more detail means a richer character and less improvisation,
and it is cheaper than it looks. A profile is the same text every call, so it is
a fixed cost, and a stable block that size is exactly what prompt caching is for.
Cached reads are about a tenth of the price, so being generous is correct.

Which detail pays, in order:

1. **Voice and behaviour beat biography.** "Answers a question with a question."
   "Goes quiet rather than lying." "Says fine when it isn't." Those do more per
   word than a life story.
2. **What they do not know.** The section everyone skips and the one that matters
   most. A profile full of what a character knows quietly tells the model it
   knows everything. One line — *she has never been below the third floor* —
   puts the character inside the world instead of above it.
3. **What they will not say.** True things they hide. The only reason to talk to
   someone twice.

Watch for exposition dumping: a rich background gets recited. That is fixed in
the prompt, not the profile.

### What writing it actually surfaced

- **Goals cannot name places.** Her motivation was "get to the deep stacks" — a
  room that does not exist and may never be generated. But *deeper* is something
  the engine can act on, because distance from the start is already computed. So
  a goal has to be written as a direction the engine can move a character in:
  deeper, outward, follows the player, stays put. Agreed to make that a short
  fixed list, the same way motives are.
- **Profiles are written before the world exists**, like the bard's overture, so
  they can only talk about the setting and never about specific rooms. State
  that to whoever writes them or the files fill with places that never get built.
- **Two rules collided and the result was better than either.** She walks toward
  danger; she will not enter a room with something hostile in it. So she stops
  one room short and waits, and she is furious about waiting. That is a
  character, produced by two plain engine rules with no AI involved.
- **The "never" section does not belong in the file.** It carries the guardrail
  that stops her explaining game rules — currently a line of text anyone can
  edit or forget. It belongs in the engine-owned prompt, applied to every
  character. The file should hold only what is true about *her*.
- **The file is really two files.** How she talks is for the model. How she moves
  is for the engine. Agreed to split the engine rules out rather than mark them
  inline.
- **"What she will not say" is dead content without a way in.** Her brother is
  the strongest thing in the profile and nothing can currently reveal him. This
  is where the promotion idea earns its place: when she does mention him, the
  bard sees it and can make him real.

## Rejected

- **The bard voices every NPC.** It already holds the whole cast and the arc, and
  one GM playing all the parts is how a real table works. It dies because it
  gives the bard a synchronous caller and puts it on the critical path,
  destroying the property that made it safe — called by nothing, safe to fail.
  This is the reason NPC voice must be its own agent.
- **Simulating every NPC every turn.** Already priced: four NPCs ≈ 16-second
  turns, and Generative Agents cost thousands of dollars for 25 agents over two
  simulated days.
- **A separate spawn-condition language for major NPCs.** Two story systems, and
  the engine's could only check what it can count.

## The open questions, settled

### 1. Talking is an action, not a mode

No conversation mode. When a character is in the room, the resolver gains one
more thing it may produce: `say`, carrying the line as written. "What's your
name" becomes speech; "take the key" becomes take. Nothing to enter, nothing to
leave.

The argument that decided it: **a talking turn costs no more than a normal one.**
A turn today is resolve, then narrate. A talking turn is resolve, then the
character's reply — printed as it comes back, the way canon room text already is.
The narrator does not run, because there is nothing to describe that the
character did not just say. Two calls either way, ~4 s either way.

Mode would have been one call rather than two, but the saving is a Haiku call
worth about a tenth of a cent, and it costs the ability to do anything else
mid-conversation.

It also settles what the world does while the player talks: talking is a turn, so
hostiles take theirs. Chatting during a fight is punished by the chip clock that
already exists. No special rule needed.

### 2. Cost — the worry does not survive decision 1

It existed only under mode, where lines would have been cheap and unlimited.
As an action, every line is a turn, and turns already cost what they cost.
Nothing new to cap.

Dialogue uses the same model as narration, because it occupies the same slot.

### 3. Putting a character into a room — already possible

`placeCatalogEntry(db, catalog, room, description, actor)` takes **any** room. It
happens to be called only from room generation today, but the helper does not
care. There is no missing capability, only a missing caller — and the bard's
commit step already writes to the world, so it can call this with a room it
chose. Small addition, not new machinery.

### 4. No room column on `events`, not yet

First version: a character remembers only conversations it had. That needs no
schema change.

When memory should include what a character witnessed, the change is cheaper
than it first looked — `appendEvent` can look up the actor's room itself, so it
is one lookup in one helper rather than an edit at all 26 call sites. And the
engine refuses to open older world files anyway, so a schema change costs a
`world.db` delete. Deferring is free.

### 5. Major NPCs cannot silently never appear

Two numbers instead of one: a tier meaning *not before here* (exists already),
and a second meaning *no later than here*. Past the second, the engine places the
character whether or not the bard chose to.

That is a default, not a condition language. The bard still owns timing
everywhere between the two, and the most expensive content in the game cannot
quietly fail to show up.

### 6. A minor NPC's identity is written on first conversation

Not at placement — most placed characters are never spoken to, and that call
would be wasted.

The first conversation call returns two things: the reply, and a short identity.
The identity is written to canon and reused forever after. Same shape the world
generator already uses, returning a room's name, description, and exits from one
call.

## Parked

- **An NPC naming a place that does not exist yet, planting a reference the world
  generator later honours.** *"The key's in the east tower"* → the east tower now
  exists as an L0 reference. Attractive, and it means talking writes to the map.
  Revisit once characters can move and the map is large enough that "go to the
  east tower" is something a player would want to act on.
- **Promoting a minor NPC to a major one** after enough conversation. Cheaper
  than it looks now that a major character is just a fuller identity plus a
  movement goal, both of which are rows. Revisit once major characters work.
- **Two NPCs in the same room.** First version: they ignore each other, and only
  the one addressed is loaded. One rule so it cannot get strange — a major
  character will not walk into a room that already holds another character, the
  same shape as the hostile rule.
- **An ally who fights.** Still a combat rebalance, not an NPC feature.

## Smallest thing that answers "how does this feel"

The generator places a friendly minor NPC in some new rooms. You can look at it
and talk to it. It answers. It remembers what you said. It does not move, act,
fight, or leave.

That tells you almost everything about how conversation feels here — whether it
is fun, whether four seconds a line is tolerable, whether they sound like people
or like a chatbot. Major characters, movement, offscreen life, and arriving
mid-scene are the second feature, and much easier to design once the first has
been played.
