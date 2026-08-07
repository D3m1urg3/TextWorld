---
title: "NPC conversation: the say action, the call, and the reply"
date: 2026-08-06
status: implemented
tags: [npc, dialogue, say, isa, resolver, prompt-caching, tool-use, fallback, latency]
modules: [nlresolve, systems, npc, prose, aihttp, mutations, render]
related: [.lore/work/design/npc-memory-store.md, .lore/work/design/examine-perception-verb.md, .lore/work/brainstorm/npcs.md, .lore/work/research/llm-npc-dialogue-and-memory.md, .lore/vision.md]
---

# NPC conversation: the say action, the call, and the reply

## Scope

The playable brick. A character in the room, a line you type, and an answer.

Builds on [examine](examine-perception-verb.md) for the wider resolver scope and
on [the memory store](npc-memory-store.md) for profiles, memory, and the
`said` / `spoke` event verbs.

Out of scope: movement, arrival, major-character placement, and the bard
promoting anything a character said into the catalog.

## Decision 1 — At most one character per room, so speech needs no addressing

Every rule already set makes this true. The room generator places at most one
catalog entry per room, and a major character will not walk into a room that
already holds one. So `say` never has to name who it is talking to: the target
is the character in the room, resolved at resolution time.

That removes an entire layer — no `talk to <name>`, no ambiguity handling, no
addressing grammar. If the one-character-per-room rule ever breaks, this is the
thing that breaks with it, and the fix is an addressing form rather than a
redesign.

**Scope**: an entity in the player's room whose catalog row has
`kind IN ('character','major')`. Beats are objects — a scorched lectern is
examinable, not conversational.

## Decision 2 — The resolver classifies; it never carries the player's words

`Verb::Say` carries `subject` (the character) and nothing else. **The text is
the player's raw input line, supplied by the engine.**

The tempting shape is a `text` argument on the tool, and it is wrong. If the
model returns the player's words, it can paraphrase them, tidy them, or
translate them — and the player would be answered for something they did not
say. Having the engine use the line it already has removes the failure entirely
rather than validating against it.

So the resolver's job on a talk turn is one bit: *is this speech, or is it one
of the other verbs?*

Three changes in `nlresolve.cpp`:

1. `say` joins the tool's verb enum and `verbFromWord` — the two lists a test
   asserts are element-wise identical.
2. The scope payload gains `present_character`: the noun of the character in the
   room, absent when there is none.
3. The system prompt gains a conditional. Today it says that a question, chatter,
   or an unknown verb means *make no tool call at all*. With a character present
   that is no longer true — chatter is an action. The rule has to be ordered:
   prefer a concrete action when the line clearly means one; fall to `say` only
   when it does not; and when no character is present, behave exactly as today.

That third change is the whole risk in this brick. It edits the rule that
governs the eleven verbs already shipping, and the failure mode is quiet —
`take the key` classified as speech looks like the character ignoring you. The
existing resolver tests must be re-run with a character present, not only with
the new phrasings.

## Decision 3 — Talking is refused while something hostile is in the room

The brainstorm's working answer was that the chip clock enforces this on its own
— talk during a fight and you get hit for it. True, but there is a better reason
to make it a rule: **it keeps a talk turn to exactly one shape.**

With no hostile present, a `say` turn produces only `said` and `spoke` events.
There is nothing to narrate, so the narrator does not run. Allowing conversation
mid-fight means a talk turn can also carry chip damage, telegraphs, and strikes,
which then need narrating — a second model call, a second code path, and an
output where a character's line and a goblin's swing have to be interleaved.

So: `say` with a hostile in the room is an engine-authored refusal, and the turn
is consumed like any other refusal the world understands.

This is not a common case by construction — friendly characters do not enter
rooms with hostiles — but the room generator can place an enemy and a character
in the same room, so it has to be handled.

## Decision 4 — One call, one tool, up to three things back

A talk turn costs the same as an ordinary turn: the resolver (fast model), then
the character (prose model). The narrator does not run. So dialogue adds no
per-turn cost over what the game already spends.

`AiRole` gains a fourth member, `Speak`, defaulting to the prose model. Dialogue
is the most quality-sensitive output in the game and it occupies the slot
narration would have used.

One tool, `reply`, with three fields:

| Field | Required | When the engine asks for it |
|---|---|---|
| `reply` | yes | always — what the character says |
| `profile` | no | only on a character's first conversation, when no profile row exists |
| `summary` | no | only when unsummarised lines have reached the fold threshold |

Two outputs from one call is the architect's shape — it already returns a room
name, its description, and its onward exits together — and it means the profile
and the memory fold never cost a call of their own.

The engine asks by varying the prompt: the fields are requested only when
wanted, and any field that arrives unrequested is ignored rather than treated as
an error.

**Fold threshold**: 20 unsummarised lines, under the store's read cap of 40, so
the bounded read never truncates in practice.

## Decision 5 — Prompt layout follows the caching rule

The research round's ordering discipline is a real constraint here, because a
character's profile is re-sent on every line and is the largest stable block in
the game.

| Position | Content | Stable? |
|---|---|---|
| system | engine-owned rules for every character | yes, globally |
| system | this character's profile | yes, per character |
| system | the setting | yes, globally |
| user | memory summary | changes |
| user | recent lines | changes |
| user | the player's line | changes |

Everything stable is in the system block; everything volatile is in the user
message. That is the cache boundary, and it is why the split falls where it
does rather than by tidiness.

One departure worth naming: every other system prompt here is a compile-time
constant (`kSystemPrompt`, `kArchitectPrompt`, `kResolveSystemPrompt`). This one
is **composed at runtime**, because the profile is per character. The
engine-owned rules stay a git-versioned constant that gets prepended, so they
remain spot-checkable by substring the way the others are.

### The engine-owned rules, and why they are not in the profile

The brainstorm moved the "never" block out of the character file. The research
gave that a threat model: players social-engineer NPCs, and a rule living in a
file an author can edit or forget is not a rule.

For this brick the real defence is structural — **a conversation cannot write to
the world.** The only rows a talk turn produces are two event rows, a profile,
and a summary. A character cannot open a door, hand over an item, or change a
number, because no code path exists for it. Persuasion has nothing to attack.

The prompt rules are defence in depth on top of that: never explain a mechanic,
never volunteer background unprompted, never name a place or person that has not
been established. The first of those is the one that matters, and it is the one
the memory store's design already explains — knowledge beats are how a weakness
becomes learnable, and they are checked against the real resistance table.
Dialogue is free prose and cannot be checked that way, so it stays out of the
mechanical business entirely.

**Record the revisit condition plainly:** the moment a character can give the
player anything, social engineering becomes live and the decision must move into
engine code.

## Decision 6 — The reply is printed verbatim, and the narrator stays out

The character's line goes to the screen as it came back, the way canon room
descriptions already do. Nothing paraphrases it.

That is not only a quality choice. It removes the parked concurrency problem
from the dungeon master session — there is no second call to run alongside,
because there is no narrator on a talk turn.

Rendering, both paths:

- **Template** — `render.cpp` gains two branches. `spoke` prints the detail;
  `said` prints nothing, because the player already saw what they typed.
- **AI** — there is no AI path for a talk turn. The reply *is* the AI output.

The status band prints as it does on every turn.

## Decision 7 — The call happens inside the tick, and failure consumes the turn

`resolveSay` makes the model call inside the tick transaction. That is the
`resolveGo` precedent — world generation already calls the architect from inside
the open transaction — so this is established practice rather than a new
liberty. It cannot borrow pre-generation's fix, either: you cannot pre-generate
an answer to a line nobody has typed yet.

On any failure — AI disabled, transport error, timeout, malformed response, no
tool call — the turn still ticks and produces:

```
said   (actor = player,  subject = character, detail = the line)
failed (detail = the engine's authored no-reply line)
```

The player spent a turn speaking and got nothing back, which is exactly how a
`go` into a wall behaves. No new mechanism, no rollback, and the fallback text
is engine-authored so it can never be a fabricated line of dialogue.

The distinction that matters, and the one only fault injection can see: a
**database fault** while writing memory or the profile rolls the turn back
rather than degrading to the no-reply line. Same boundary the materialisation
brick drew, and it needs the same kind of test.

A malformed `profile` or `summary` field is dropped and the reply still lands —
the leniency materialisation already established, where a bad part never costs
the whole turn.

## Decision 8 — Profiles are write-once, so first contact is the only chance

The first conversation with a minor character is where its profile gets written,
and the store makes that permanent. A second profile field in a later response
is ignored, silently, because `writeCatalogProfile` is a one-way latch.

If that first call fails, no profile is written and the next conversation asks
again. So a failed first contact costs nothing permanent, which is the right
default.

The cost of write-once is that a bland first profile is bland forever. That is
the same bet the project makes about room descriptions, and it should be
revisited only if characters actually come out flat.

## What the player's words in the database mean

`said` rows carry the player's typed input, so the world file now contains what
the player wrote. That is intended — it is the memory — but it is a new category
of content in `world.db` and worth stating, because the session log deliberately
does the opposite and never records typed input above debug level.

One consequence arrives free: the bard's wake context reads recent events, so a
waking bard will see conversations. That is exactly the promotion path the
brainstorm wanted — a character mentions a brother, the bard can later make the
brother real. Nothing here builds it, and speech does **not** become a wake
trigger: `said` and `spoke` are not irreversible, and the bard still wakes only
on `generated`, `defeated`, `learned`, and `materialized`.

## Testing shape

Offline, fake transports, no key — the standard here.

- Refusals: no character in the room; a hostile in the room. Exact strings.
- The one-bit resolver change: existing phrasings for all eleven verbs re-run
  **with a character present**, asserting none of them becomes `say`.
- The enum / `verbFromWord` equality assertion, extended.
- Caching claim as a test: two calls to the same character with different memory
  produce **byte-identical system blocks**.
- First contact requests a profile; the second conversation does not.
- Write-once: a profile field in a second response changes nothing.
- Fold threshold: `summary` requested at 20 unsummarised lines and not at 19.
- The reply reaches the screen byte-for-byte, and **no narrate request is
  issued** on a talk turn.
- Every failure mode produces `said` + `failed` with the authored line, the turn
  counter advances, and the output is byte-identical across all of them.
- Fault injection: a database error writing memory rolls the turn back, with a
  control arm proving the injection touches nothing else.

## Decision summary

1. **One character per room**, guaranteed by existing rules, so `say` needs no
   addressing grammar.
2. **The resolver classifies only.** `Verb::Say` carries the character; the text
   is the engine's copy of the player's line, never the model's.
3. **Refused while a hostile is present**, which keeps a talk turn to exactly
   one shape and one call.
4. **One tool call returns the reply, plus a profile on first contact and a
   summary at the fold threshold** — the architect's two-outputs-one-call shape,
   so memory work never costs a call.
5. **Stable content in the system block, volatile in the user message**, which
   is the caching rule made structural; the prompt is composed at runtime
   because the profile is per character.
6. **The reply prints verbatim and the narrator does not run**, which also
   dissolves the parked concurrency question.
7. **The call is inside the tick; failure consumes the turn** and produces an
   engine-authored no-reply line. A database fault rolls back instead.
8. **`AiRole` gains `Speak`**, on the prose model.
9. **A conversation cannot write to the world.** That, not the prompt, is what
   makes social engineering impossible — and the revisit condition is the day a
   character can give the player something.
