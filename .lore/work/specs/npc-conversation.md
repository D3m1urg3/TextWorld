---
title: "NPC conversation: requirements"
date: 2026-08-06
status: implemented
tags: [npc, dialogue, say, isa, resolver, prompt-caching, tool-use, fallback, latency, requirements]
modules: [nlresolve, systems, npc, aihttp, mutations, render, action]
related: [.lore/work/design/npc-conversation.md, .lore/work/specs/npc-memory-store.md, .lore/work/specs/examine-perception-verb.md, .lore/work/research/llm-npc-dialogue-and-memory.md, .lore/work/brainstorm/npcs.md]
req-prefix: NPCTALK
---

# NPC conversation: requirements

## Context

The playable brick: a character in the room, a line you type, and an answer.
Established in [the design](../design/npc-conversation.md).

Depends on [examine](examine-perception-verb.md) for the resolver's wider scope
payload, and on [the memory store](npc-memory-store.md) for profiles, memory,
the `said` / `spoke` verbs, and the read helpers.

**The load-bearing property.** A conversation **cannot write to the world**. The
only rows a talk turn produces are two event rows, at most one profile, and at
most one memory summary. A character cannot open a door, hand over an item, or
change a number, because no code path exists for it. The research round named
the failure this prevents: players social-engineer NPCs into surrendering
quest-critical items, which decouples persuasion from the game's own systems.
Prompt rules are defence in depth on top of that; the structure is the defence.

**Scope boundary.** Movement, arrival, major-character placement, and the bard
promoting anything a character said into the catalog are out of scope.

## Requirements

### Target and scope

**REQ-NPCTALK-1** — A room holds at most one character, guaranteed by rules
already set: the generator places at most one catalog entry per room, and a major
character will not enter a room that already holds one. `say` therefore needs no
addressing grammar and no ambiguity handling.

**REQ-NPCTALK-2** — The target is the entity in the player's current room whose
catalog row has `kind IN ('character','major')`, resolved at resolution time.
Beats are not conversational — a scorched lectern is examinable, not talkable.

**REQ-NPCTALK-3** — With no character in the room, `say` produces a `failed`
event carrying an engine-authored line. The turn is consumed, as it is for every
refusal the world understands.

**REQ-NPCTALK-4** — With a hostile in the room, `say` produces a `failed` event
carrying a different engine-authored line, and the turn is consumed. This is a
rule, not a consequence of the chip clock: it keeps a talk turn to exactly one
shape — `said`/`spoke` only, nothing to narrate, one model call. It is reachable
because the generator can place an enemy and a character in the same room.

**REQ-NPCTALK-4a** — The two refusals are checked in a fixed order:
**REQ-NPCTALK-3 first**. An empty room that contains a hostile yields the
no-one-here line, not the hostile line, because with nobody present that is the
truthful answer and "you can't talk during a fight" would imply there was
someone to talk to.

### The ISA

**REQ-NPCTALK-5** — `Verb` gains `Say`, carrying `subject` = the character. The
verb set becomes thirteen (twelve after [examine](examine-perception-verb.md),
plus this).

**REQ-NPCTALK-6** — **The player's words are never carried by the model.** `Say`
has no text payload on the wire in either direction. The engine uses the raw
input line it already holds. A `text` argument on the tool would let the model
paraphrase, tidy, or translate what the player typed, and the character would
then answer something the player did not say — a failure removed by construction
rather than validated against.

**REQ-NPCTALK-7** — `Say` reaches `resolve` inside the tick transaction, after
`meta.turn` has been incremented, like every other world-touching verb.

### The deterministic parser

**REQ-NPCTALK-8** — The fixed-verb parser recognises `say <text>`, taking the
remainder of the line verbatim as the spoken text. Bare `say` returns `nullopt`
(REQ-PROTO-6a).

**REQ-NPCTALK-9** — The parser does **not** resolve the target. It emits `Say`
with `subject = 0`; resolution finds the character in the room, the same shape
`attack` already uses for the hostile.

**REQ-NPCTALK-10** — With AI disabled, `say` still parses and still reaches
resolution, and resolution produces REQ-NPCTALK-19's no-reply refusal. The
parser never branches on AI availability.

### The resolver

**REQ-NPCTALK-11** — `say` joins the `emit_action` tool's verb enum and
`verbFromWord`, keeping the two lists element-wise identical.

**REQ-NPCTALK-12** — The scope payload gains `present_character`: the noun of the
character in the room, **absent** when there is none — not present-and-empty.

**REQ-NPCTALK-13** — The tool's `say` case takes **no argument**. `subject` is
not requested and is ignored if supplied, following REQ-NPCTALK-6.

**REQ-NPCTALK-14** — The system prompt's rule that a question, chatter, or an
unknown verb means *make no tool call at all* becomes conditional. With
`present_character` absent, behaviour is **unchanged**. With it present, the
ordering is: prefer a concrete action when the line clearly means one; fall to
`say` only when it does not.

**REQ-NPCTALK-15** — REQ-NPCTALK-14 is the highest-risk change in this spec. It
edits the rule governing the twelve verbs already shipping, and its failure is
quiet — `take the key` classified as speech reads as the character ignoring you.
Validation therefore re-runs the existing phrasing tests **with a character
present**, not only the new phrasings.

**REQ-NPCTALK-15a** — The action-versus-speech boundary is **prompt-tuning
territory and is stated as such**. No wording makes it crisp for every input, and
two prompts could both pass validation while disagreeing on unlisted phrasings.
What is contractual is the **named regression set** in validation item 6; what
lies outside it is tuned against real play, not specified. This is recorded so a
later reader does not mistake a passing suite for a settled rule.

Both directions of misclassification are recoverable by retyping, so the failure
is annoying rather than destructive — which is why an imprecise boundary is
acceptable here and would not be for a rule that wrote to the world.

### The call

**REQ-NPCTALK-16** — `AiRole` gains `Speak`, defaulting to the prose model.
`TEXTWORLD_MODEL` overrides it along with every other role, unchanged.

**REQ-NPCTALK-17** — Exactly **one** model call per talk turn, plus the resolver
call every turn already pays. The narrator does **not** run on a talk turn, so
dialogue adds no per-turn cost over an ordinary turn.

**REQ-NPCTALK-18** — One tool, with three fields:

| Field | Required | Requested when |
|---|---|---|
| `reply` | yes | always |
| `profile` | no | the character has no `catalog_profile` row |
| `summary` | no | unsummarised lines have reached the fold threshold |

The engine asks by varying the prompt. A field arriving **unrequested is
ignored**, never an error — the leniency materialisation already established.

**REQ-NPCTALK-18a** — The profile condition is stated over the *row*, not over
the character's kind, and that is deliberate. A major character always has a
profile from world creation (REQ-NPCSTORE-33), so in practice the request is a
minor-character path — but a major that somehow lacks one is handled by the same
branch rather than by a special case that would have to be discovered. The
condition is "no `catalog_profile` row", never "kind is character".

**REQ-NPCTALK-19** — The fold threshold is **20** unsummarised lines, under the
store's `kLineCap` of 40, so the bounded read never truncates in practice.

### Prompt layout

**REQ-NPCTALK-20** — Content is split by stability, not by tidiness:

| Block | Content | Stable across calls |
|---|---|---|
| system | engine-owned rules for every character | globally |
| system | who this character is: name and canon description | per character |
| system | this character's profile | per character |
| system | the setting | globally |
| user | memory summary | no |
| user | recent lines | no |
| user | the player's line | no |

> **Amended during implementation.** The second row was added. `npcProfile` is
> empty *by definition* on a minor character's first contact — the one call that
> writes the profile — so a three-row block would hand the model rules, nothing,
> and the setting, and it would invent a character unrelated to the figure the
> player is looking at. `writeCatalogProfile` is a one-way latch
> (REQ-NPCTALK-35), so that invention would be permanent. The name and canon
> description are already on the player's screen, are stable per character (so
> REQ-NPCTALK-21 holds unchanged), and are neither ids, tiers, flags, nor
> internal tags (so REQ-NPCTALK-24 holds unchanged).

**REQ-NPCTALK-21** — The system block is **byte-identical** across two
conversations with the same character whose memory differs. That is the cache
prefix, and it is a testable property rather than an intention.

**REQ-NPCTALK-22** — This system prompt is **composed at runtime**, unlike every
other one here, because the profile is per character. The engine-owned rules stay
a git-versioned string constant that is prepended, so they remain spot-checkable
by substring the way `kArchitectPrompt` and `kResolveSystemPrompt` are.

**REQ-NPCTALK-23** — The engine-owned rules forbid, for every character: explaining
any mechanic (a weakness, damage, a cooldown, a resistance); volunteering
background unprompted; and naming a place, person, or object not already
established. They are **not** in the character's profile file, because a rule a
player will actively attack cannot live in a file an author can edit or forget.

**REQ-NPCTALK-24** — No entity id, catalog id, tier, `seeded` flag, or
engine-internal tag appears anywhere in the request body. Event lines pass through
the same shield that keeps machine tokens out of narration.

### The reply

**REQ-NPCTALK-25** — The reply is printed **verbatim**, exactly as returned.
Nothing paraphrases, wraps, or rewords it — the treatment canon room descriptions
already get.

**REQ-NPCTALK-26** — `render.cpp` gains two branches: `spoke` prints its detail;
`said` prints **nothing**, because the player already saw what they typed. Every
other verb's template output stays byte-identical.

**REQ-NPCTALK-27** — There is no AI narration path for a talk turn. The reply is
the AI output. No narrate request is issued.

**REQ-NPCTALK-28** — The status band prints as on every other turn, unchanged.

### Writes and failure

**REQ-NPCTALK-29** — A successful talk turn appends `said` then `spoke`, in that
order, in one transaction. It may also write one profile (first contact) and one
memory summary (at the threshold), both through the store's helpers. Where the
profile and summary writes fall relative to the two event rows is **immaterial**
— the transaction is atomic and nothing reads them mid-turn — but `said` before
`spoke` is required, because `npcLinesSince` returns rows in log order and a
reply preceding its question would be read back as one.

**REQ-NPCTALK-29a** — `writeNpcMemory` stamps `summary_turn` with **the turn
before the current one**, not the current turn. `npcLinesSince` filters on
`turn > summary_turn`, so stamping the current turn would make this turn's
`said`/`spoke` invisible to every future read — and they are **not** in the
summary either, because the model wrote it from the lines it was given plus an
input, before producing the reply. Stamping one turn back keeps the exchange
that was just produced visible until the *next* fold consumes it. This is the
one place where an off-by-one silently loses a conversation instead of failing.

**REQ-NPCTALK-30** — The model call happens **inside** the tick transaction. This
is the `resolveGo` precedent — world generation already calls the architect from
inside the open transaction — and pre-generation's fix is unavailable here, since
a reply to an unheard line cannot be prepared in advance.

**REQ-NPCTALK-31** — On **any** AI failure, the turn still ticks and produces the
rows below. The enumerated cases are exactly eight: AI disabled; transport error
(status 0); timeout; a non-200 status; an unparseable body; a well-formed
response with no tool call; a tool call missing `reply`; a tool call with an
empty `reply`.

```
said   (actor = player, subject = character, detail = the line)
failed (detail = the engine's authored no-reply line)
```

The fallback text is engine-authored, so a failure can never surface as a
fabricated line of dialogue.

**REQ-NPCTALK-32** — A malformed `profile` or `summary` field is **dropped** and
the reply still lands. A bad part never costs the whole turn.

**REQ-NPCTALK-33** — A **database fault** while writing the event rows, the
profile, or the memory **rolls the turn back** rather than degrading to the
no-reply line. This is the distinction only fault injection can see, and it is
the same boundary materialisation drew.

**REQ-NPCTALK-34** — A failed first contact writes no profile, and the next
conversation asks for one again. Nothing permanent is lost by a failure.

**REQ-NPCTALK-35** — A second `profile` in a later response changes nothing,
silently, because `writeCatalogProfile` is a one-way latch. The consequence — a
bland first profile is permanent — is accepted, being the same bet this project
makes about room descriptions.

### Consequences stated, not built

**REQ-NPCTALK-36** — The bard's wake context reads recent events, so a waking
bard sees conversations for free. Nothing in this spec acts on that; it is the
promotion path a later brick uses.

**REQ-NPCTALK-37** — `said` and `spoke` do **not** become wake triggers. The
predicate stays `generated`, `defeated`, `learned`, `materialized`.

**REQ-NPCTALK-38** — The revisit condition, recorded so it is not rediscovered:
**the day a character can give the player anything**, social engineering becomes
live and the decision must move into engine code rather than the prompt.

## AI Validation

Offline throughout — fake transports, no network, no `ANTHROPIC_API_KEY`.

### Refusals and scope

1. `say hello` with no character in the room → the exact no-one-here line, and
   `meta.turn` advanced by one.
2. `say hello` with a character **and** a hostile present → the exact
   hostile-present line, `meta.turn` advanced, and **no model call issued**.
3. A room containing only a `beat` entity → the no-one-here line; beats are not
   targets.
3a. A room with **no character but a hostile present** → the no-one-here line,
   not the hostile line (REQ-NPCTALK-4a).
3b. The `said` event's `turn` column equals the incremented `meta.turn`, which
   is what makes REQ-NPCTALK-7's ordering claim observable rather than a
   statement about code structure.
4. A character present → the call is made and the reply lands.

### The resolver

5. Enum and `verbFromWord` element-wise equal, extended to thirteen.
6. **The regression that matters:** every existing phrasing test for the twelve
   prior verbs re-run against a room supplying `present_character`, asserting
   none resolves to `say`. Specifically `take the key`, `go north`, `attack`,
   `cast ward`, `read grimoire`, `examine candle`, `look`, `inventory`.
7. With `present_character` absent, a question or chatter still produces **no
   tool call** — behaviour byte-identical to before this spec.
8. With it present, the same question resolves to `say`.
9. A captured body shows `present_character` **absent** in a room with no
   character, not present-and-empty.
10. A `say` tool call carrying a `subject` or a text argument is accepted, and
    the argument is ignored — the spoken text in the resulting `said` row is the
    player's raw line, byte-equal.

### The call and prompt

11. Two calls to the same character with different memory produce
    **byte-identical system blocks** (REQ-NPCTALK-21).
12. The system block contains the engine-rules constant, the profile, and the
    setting; the user message contains the summary, the lines, and the input.
13. A body sweep asserts no integer id, tier, `seeded` flag, or internal tag
    appears anywhere in the request.
14. The engine-rules constant is spot-checkable by substring for each of
    REQ-NPCTALK-23's three prohibitions.
15. Exactly one `Speak` request is issued per talk turn, and **no `Narrate`
    request is issued** — asserted by counting requests by role, not by absence
    of output.
16. `Speak` uses the prose model by default and honours `TEXTWORLD_MODEL`.

### Fields and thresholds

17. First contact: the prompt requests a profile; the response's profile is
    stored; the reply still prints.
18. Second conversation: the prompt does **not** request a profile, and a profile
    supplied anyway changes nothing (assert the stored text is unchanged).
19. `summary` is requested at 20 unsummarised lines and not at 19.
19a. **The off-by-one that loses a conversation** (REQ-NPCTALK-29a): after a
    turn that writes a summary, `npcLinesSince` still returns that turn's `said`
    and `spoke`. Written to fail if `summary_turn` is ever stamped with the
    current turn instead of the one before it.
19b. A major character is never asked for a profile, because one exists from
    world creation — and a major with its profile row deleted **is** asked, via
    the same branch rather than a special case (REQ-NPCTALK-18a).
20. A malformed `profile` is dropped and the reply still lands.
21. A malformed `summary` is dropped and the reply still lands.

### Failure

22. Each of REQ-NPCTALK-31's **eight** cases — AI disabled, transport error
    (status 0), timeout, status 500, unparseable body, no tool call, missing
    `reply`, empty `reply` — produces `said` + `failed` with the exact authored
    line, `meta.turn` advanced, and **byte-identical output across all eight**.
    Timeout is driven as its own case rather than collapsed into transport
    error, because the two reach the failure through different code.
23. No failure path ever produces text that could be mistaken for dialogue —
    assert the fallback line is the engine constant.
24. Fault injection: a database error writing `npc_memory` rolls the turn back —
    no `said` row, no `spoke` row, `meta.turn` unchanged — with a **control arm**
    proving the injection touches nothing else.
25. **Mutation check:** swallowing that exception, the exact downgrade
    REQ-NPCTALK-33 forbids, turns the suite red.
26. A failed first contact leaves no profile row, and the following conversation
    requests one again.

### Rendering

27. `spoke` prints its detail byte-exact; `said` prints nothing.
28. The reply reaches the screen byte-for-byte, including punctuation and
    internal newlines.
29. **Regression:** a scripted session exercising every other verb produces
    byte-identical output to a run before this change.
30. The status band appears on a talk turn, composed identically to any other
    turn.

### Whole-spec gates

31. `grep -En "INSERT|UPDATE|DELETE"` over the new conversation translation unit
    is empty — every write goes through `mutations.cpp`, the discipline
    `architect.cpp` and `bard.cpp` already live under.
32. A talk turn writes **no** component-table row: no `location`, `health`,
    `known_spells`, `hostile`, or `exits` change. Asserted by row-count
    comparison across the turn, which is the mechanical form of the
    social-engineering defence.
33. The suite passes with no network and no API key.
34. Every existing resolver, narrator, architect, bard, and combat test passes
    **unmodified**.

## Out of scope

- Movement, goals, arrival, and major-character placement.
- The bard acting on what it reads in a conversation.
- Talking to more than one character, or characters talking to each other.
- Streaming the reply. It is the strongest latency lever this feature has —
  waiting four seconds for a room description is tolerable, waiting four seconds
  for a person to answer you is not — and it stays on the deferred list from the
  latency work.
