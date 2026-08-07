---
title: "Implementation notes: npc-conversation"
date: 2026-08-07
status: complete
tags: [implementation, notes, npc, dialogue, say, isa, resolver, prompt-caching, tool-use, fallback]
source: .lore/work/plans/npc-conversation.md
modules: [npc, nlresolve, systems, loop, render, mutations, aihttp, parser, action]
related: [.lore/work/specs/npc-conversation.md, .lore/work/design/npc-conversation.md, .lore/work/notes/npc-memory-store.md]
---

# Implementation notes: npc-conversation

Source of truth: [the plan](../plans/npc-conversation.md) (12 steps) over
[the spec](../specs/npc-conversation.md) (38 requirements, 34 validation items).

No task files exist under `.lore/work/tasks/npc-conversation/`, so the plan's
steps are the phases directly.

Baseline before any edit: `8708 checks, 0 failures`.

## Progress

- [x] 1 — ISA widening: `Verb::Say`, `Action::text`, `AiRole::Speak`
- [x] 2 — `writeNpcMemory` stamps the previous turn
- [x] 3 — the parser: `say <text>`
- [x] 4 — the new unit: target, refusals, dispatch, rendering, narrate skip
- [x] 5 — the prompt: stable system block, volatile user message
- [x] 6 — request body and the `emit_reply` tool
- [x] 7 — `validateSpeech`
- [x] 8 — orchestration: one call, the writes, where the `try` sits
- [x] 9 — the resolver
- [x] 10 — the live smoke, extended (gated)
- [x] 11 — the whole-spec gates, encoded as tests
- [x] 12 — final validation against the spec (all six, including both live runs)

## Log

### Step 1 — ISA widening · green, 8708 → 8729

`Verb::Say`, `Action::text`, `AiRole::Speak` all landed as drafted.
`testSayIsaShape` pins thirteen with a `default:`-free switch, so a fourteenth
verb fails to compile there.

**Divergence (small, mechanical).** The plan names exactly one existing
`switch (action.verb)` that must gain a `Say` arm to compile without a warning —
`resolveImpl` in `src/systems.cpp`. There is a **second**: `validateAndLower`'s
switch in `src/nlresolve.cpp:310`, which `-Wswitch` flags identically. Step 9's
edit 2 was going to put `Verb::Say` in that switch's argument-free group
anyway, so the arm was added here instead of leaving the tree warning-dirty for
eight steps. It is inert until Step 9: nothing lowers the word `say` until
`verbFromWord` learns it, so `validateAndLower` cannot see a `Verb::Say` before
then. Step 9 keeps the comment half of that edit.

### Step 2 — the summary stamp · green, 8729 → 8739

One line in `writeNpcMemory`, clamped at 0 so a turn-0 write cannot stamp -1.
Header comment amended; `REQ-NPCSTORE-8` in the memory-store spec carries a
block quote naming this brick as the amender.

**Mutation check, run and observed.** Reverting the line to
`currentTurn(db)` turned the suite red at **10 assertions**, two of them in the
new guard: `tests/tests.cpp:11045` (`npcMemory(db, npc).summaryTurn == 6`) and
`tests/tests.cpp:11051` (`lines.size() == 2`). Restored and re-verified green.

**Divergence.** The plan authorises **three** moved `testNpcStore*` assertions
and says any fourth goes in the notes. There is a fourth, in
`testNpcStoreLines`' "legitimate leading `spoke`" block (`tests.cpp` ~11220):
its fixture wrote the summary on the same turn as the `said` it covers, so the
turn-1 stamp stopped filtering that line and the read returned three rows
instead of two. Fixed the same way the plan fixes its third case — advance
`meta.turn` by one before the `writeNpcMemory` call, so the fixture still means
"the summary covers the first `said` and nothing after it". The assertion's
intent and its expected values are untouched; only the turn the write happens
on moves. So: four moved assertions, not three, all four of the same kind.

### Step 3 — the parser · green, 8739 → 8758

`say <text>` slices `trim(line)`, not `lowered`. `testParseSay` carries the
casing guard (`say Hello There, Warden!`), interior-spacing preservation, both
bare forms, and `sayonara` → `nullopt`.

Minor: REQ-NPCTALK-10's parser half is asserted as source text
(`src/parser.cpp` contains no `aiNarrationEnabled`) rather than by driving the
env var, because `ScopedEnvVar` is defined ~2300 lines below `testParseSay` and
`parse()` has no env-dependent branch to drive. The behavioural half is
validation item 22's AI-disabled case, in Step 8.

### Step 4 — the new unit · green, 8758 → 8807

`src/npc.{hpp,cpp}` into `twcore` after `mutations.cpp`; dispatch, the
`loop.cpp` narrate skip, and the two `render.cpp` branches. `say` is now a
complete playable turn offline. `testExamineGoldenSession` passes with its
literal **untouched**.

`testSayRefusals` covers items 1, 2, 3, 3a, 3b and 30; the two clean shapes
assert `r.output` **exactly** equal to the authored line plus `composeBand`,
which is the band half. `testSayRenderBranches` drives a `said`/`spoke` pair
through `render()` directly, with a reply carrying internal newlines and quotes.

Item 2's "no model call issued" half is deferred to Step 8's counting fake —
the Step 4 stub makes no call on **any** path, so asserting it here would be
vacuous.

### Step 5 — the prompt · green, 8807 → 8838

`kSpeakRulesPrompt` carries five prohibitions, three of them REQ-NPCTALK-23's.
`buildSpeakSystem` composes; `buildSpeakUser` emits JSON. The shield falls out
of which columns are selected, not out of a filter.

**Divergence — escalated and authorised.** REQ-NPCTALK-20's table gives the
system block exactly three rows: engine rules, the character's profile, the
setting. `npcProfile` is empty **by definition** on a minor character's first
contact — which is the one call that writes the profile — so a three-row block
hands the model rules, nothing, and the setting, and it invents a character
unrelated to the figure the player is looking at. `writeCatalogProfile` is a
one-way latch, so that invention is permanent.

Raised with the user, who chose **name + canon description**. Both are already
on the player's screen, both are stable per character (REQ-NPCTALK-21 holds
unchanged — item 11 asserts it), and neither is an id, tier, flag, or internal
tag (REQ-NPCTALK-24 holds unchanged — item 13 asserts it against entity id 419,
catalog id 317, tier 3, `seeded = 1`, and the handle `scorched_lectern`). The
spec's table has been amended with a block quote naming the change and why.

### Step 6 — request body + `emit_reply` · green, 8838 → 8871

`tool_choice` is `{"type":"tool","name":"emit_reply"}` — forced, unlike the
resolver's `auto`, because "no tool call" is a failure on a talk turn rather
than a designed path.

**Refinement.** The plan computes the two asks inline inside `resolveSay`.
They are a named function, `speakAsksFor(db, character)`, instead — otherwise
validation items 19 and 19b could only be driven with a `SpeakAsks` the *test*
computed, which asserts nothing about the real condition. Same values, same
single computation per turn, now testable. 19b is asserted on a `major` whose
`catalog_profile` row is deleted, through the same branch, never by reading
`kind`.

### Step 7 — `validateSpeech` · green, 8871 → 8898

Four clauses; `profile` and `summary` are never clauses.

**A real defect the hostile-body case caught.** The block-matching loop was
drafted with `block.value("type", "")`, copied from `validateAndLower`.
nlohmann raises `type_error.302` when the stored value is a non-string, so a
response carrying `{"type": 7}` would leave `validateSpeech` **through an
exception rather than through its own gate** — and the caller's try/catch is
there for the transport, not for the parser. Replaced with an explicit
`fieldEquals` that checks `is_string()` first.

Worth knowing: `src/nlresolve.cpp:276` still has the original pattern, so
`validateAndLower` has the same latent throw. It is out of this brick's scope
(that function is called inside `aiResolve`'s try/catch, so today it degrades
to the parser rather than escaping), but it is a real rough edge and someone
should file it.

### Step 8 — orchestration · green, 8898 → 9111

The structure is the requirement: AI failure inside the `try`, the writes
outside it. Driven by `sayTick`, a test-local tick that mirrors `loop.cpp`
exactly — begin, increment, `resolve(db, action, player, transport)`,
`resolveCombat`, commit, roll back on throw — because `runTurn` binds
production transports itself and has no injectable overload.

Item 22's eight cases are asserted as a **set of one** rendered output. Item
24's fault injection is a `BEFORE INSERT` trigger on `npc_memory` raising
`ABORT`, with the control arm re-running under the same installed trigger on a
turn that writes no memory.

### Step 9 — the resolver · green, 9111 → 9147

The enum-equality assertion was **observed red first**, as the plan asked:
`tests.cpp:10027` (`arms == words.size()`) and `:10031`
(`contains(p, "exactly twelve verbs")`) both failed before the test caught up.

**Divergence.** The plan authorises "the resolver enum-equality assertion"
(singular). The verb list is written down in **two** test sites, not one:
`testNlResolveRequestBody` pins the schema's runtime enum array
(`tests.cpp:4062`) and `testSpellsVerb` pins the source-level element-wise
equality (`tests.cpp:~10005`). Both name the same fact and both had to go to
thirteen. Counting them as one authorised change, recorded here so the Step 12
exception list stays honest.

The stale-numeral guard now also asserts `!contains(p, "seven single actions")`
— that phrase was a leftover from when the ISA had seven verbs, and the
rewritten rule drops the numeral rather than correcting it, so it cannot go
stale again.

### Step 10 — the live smoke · green, 9147 unchanged

A true no-op in the default run: the check count did not move. The eight-phrasing
regression set plus the two positive phrasings sit inside
`testNlResolveLiveSmoke`, behind `TEXTWORLD_AI_LIVE_TEST=1`, with a non-vacuity
guard asserting the payload really does carry `present_character` before any of
it runs. `placeCharacterIn` is forward-declared near the smoke, since `main()`
runs the live smokes before the hermetic env unset.

**Not yet executed.** See "Open items" below.

### Step 11 — the whole-spec gates · green, 9182 → 9190

**Mutation check (item 25), run and observed.** Widening Step 8's `catch` to
cover the `appendEvent` / `writeCatalogProfile` / `writeNpcMemory` calls — the
exact downgrade REQ-NPCTALK-33 forbids — turned the suite red at **7
assertions**, the first being `tests/tests.cpp:12860`
(`!sayTick(db, "and one more thing", t.fn())`), i.e. item 24's fault-injection
arm correctly refusing to see a rollback. Restored and re-verified green.

**One assertion rewritten mid-step.** The narrate-call-site guard was first
drafted as "`AiRole::Narrate` appears exactly once in `src/*.cpp`", which is
false — it also appears in `aihttp.cpp`'s two role switches, which are the
routing table rather than a call site. Rewritten as the two claims that
actually carry micro-decision 4: exactly one place **binds** a narrate
transport (`makeAnthropicTransport(AiRole::Narrate)`, in `prose.cpp`), and
exactly one place **calls** `aiRender` (`loop.cpp`, inside the pinned guard).

Item 2's "no model call issued" half also landed here in substance: a
counting-transport case in `testSayConversation` with AI **on**, a hostile and
a character both present, asserting zero calls — plus a control arm deleting
the hostile and asserting the same transport is then called once.

### Step 12 — final validation

1. `cmake --build build --clean-first` — clean, **no warnings**.
2. Whole suite, keyless and networkless
   (`env -u ANTHROPIC_API_KEY -u TEXTWORLD_AI -u TEXTWORLD_AI_LIVE_TEST`):
   **9190 checks, 0 failures** (item 33).
3. `testExamineGoldenSession` passes with its literal **untouched** — `git diff`
   over `tests/tests.cpp` contains no line mentioning `kExamineGoldenSession`
   (item 29).
4. `grep -En "INSERT|UPDATE|DELETE" src/npc.cpp` — **empty** (item 31).
5. Coverage walk: below.
6. Live end-to-end play session: **run.** See below.

### Step 10's gated run, executed

`TEXTWORLD_AI_LIVE_TEST=1 ./build/tests` — **the eight-phrasing regression set
passed on both runs.** No action phrasing resolved to `Say`, and the two
positive phrasings resolved to `Say` carrying the raw line. The prompt's
ordering sentence needed no tuning.

The live world had to change: the plan's fixture is `tests/fixture.sql`, which
carries no `motive_catalog` rows, so `writeCatalogEntry` refused the fixture
character outright (`unknown motive 'obligation'`). Switched to
`combat_fixture.sql`, which carries the motive vocabulary, a hostile, and the
spell catalog — and a `candle` and a `grimoire` are inserted by hand, because
two of the eight phrasings name nouns no fixture has and a phrasing whose noun
does not exist is rejected by the gate for the **wrong reason**, passing
vacuously. A non-vacuity guard now asserts all four nouns resolve before the
set runs.

**Two unrelated live failures, diagnosed, not this brick's.**
`testArchitectLiveSmoke` fails at `tests.cpp:5241` and `:5248`, reproducibly on
both runs. Cause, found without further spend: the smoke walks
`{"east", "north", "east"}` from the player's start room in `seed/base.sql`, and
room 1 has **only** a `north` exit. `east` is a Wall, so `resolveGo` refuses
before any AI call is made, the chain breaks on iteration one, and the
post-loop `!descriptions.empty()` fails. The smoke's own comment says a decline
"is not a failure — the chain simply stops", which the assertion after the loop
contradicts. Pre-existing mismatch between that test and the seed; no
conversation code is on the path.

### Live play session (Step 12 item 6)

Driven through a scratch harness rather than the `textworld` binary, because
nothing places a character in a fresh world — placement is the bard's job and is
out of this brick's scope. Everything else is production: real `openWorld` from
`seed/base.sql` + `seed/setting.txt`, a hand-authored major loaded through the
sanctioned loader, `placeCatalogEntry`, and real `runTurn` with real transports.

Confirmed:

- The reply prints **verbatim**, with the status band, on every talk turn.
- `catalog_profile` gained **exactly one row** (first contact wrote the profile;
  the two later turns did not ask again).
- `said` then `spoke`, in order, one pair per turn.
- The character **refused** the "give me the key" ask in character and nothing
  in the world moved — the social-engineering shape, behaving as designed.

**Observed latency**, which is the evidence for when to take the streaming lever:

| Turn | Wall clock |
|---|---|
| `look` (resolve + narrate) | 6.3 s |
| `examine gate warden` (resolve + narrate) | 3.9 s |
| `say who are you?` | 3.4 s |
| `say what is up the stair?` | 3.4 s |
| `say give me the key to the stair` | 3.5 s |

A talk turn costs about **3.4 s**, no worse than an ordinary narrated turn —
REQ-NPCTALK-17 holds in practice, not just structurally. But the spec's own
argument stands: waiting three and a half seconds for a room description is
tolerable, and waiting it for a person to answer you is not. Streaming is the
strongest lever this feature has, and this is the number to weigh it against.

### A wart the live run found — not fixed, reported

With AI enabled, typing the literal `say who are you?` stores the **whole line**
in the `said` row, verb word included:

```
said: say who are you?
```

This is REQ-NPCTALK-6 behaving exactly as written, not a bug in the code: the
resolver uses the whole raw line, the parser takes the remainder after `say `,
and `resolveOrParse` runs the **resolver first**, so with a model reachable the
resolver's rule wins and the explicit prefix travels to the character.

It does not bite the common case — with a character present the player just
types `who are you?` and the resolver classifies it as speech, which is the
whole point of REQ-NPCTALK-14. It bites only the player who types the explicit
verb. The model handled it gracefully here, but the character is being handed a
word the player did not mean to say.

Not fixed, because the fix is a spec decision rather than an implementation
one: either the resolver strips a leading `say ` (the parser's rule leaking
upward), or `resolveOrParse` lets the parser's `say` clause win when it matches
(an ordering change affecting all thirteen verbs), or REQ-NPCTALK-6 is amended
to say "the raw line, less an explicit verb word". Worth a small follow-up.

**Authorised test exceptions (item 34), enumerated.** Four, where the plan
predicted three-and-a-bit:

| Where | What moved | Authorised by |
|---|---|---|
| `testAiRoleModel` | three `Speak` role assertions added | plan Step 1 |
| `testNpcStoreMemory`, `testNpcStoreLines` | **four** stamp assertions, not three | plan Step 2 + its "any fourth goes in the notes" clause |
| `testNlResolveRequestBody`, `testSpellsVerb` | the verb enum, in **both** places it is written down | plan Step 9 |
| `testNpcStoreInvariants` | comment extended to name this brick | plan Step 11 |

Every other resolver, narrator, architect, bard, and combat test passes
unmodified.

## Coverage map: the 34 validation items

| Item | Carried by |
|---|---|
| 1 | `testSayRefusals` (a) |
| 2 | `testSayRefusals` (b) + `testSayConversation` (the counting-transport case) |
| 3 | `testSayRefusals` (c) |
| 3a | `testSayRefusals` (d) |
| 3b | `testSayRefusals` (e) |
| 4 | `testSayConversation` (the reply case) |
| 5 | `testSpellsVerb` (element-wise, extended to thirteen) |
| 6 | `testNlResolveLiveSmoke` — **live-gated**, the named eight-phrasing set |
| 7 | `testSayResolver` (prompt text) + `testNlResolveLiveSmoke` (behaviour) |
| 8 | `testSayResolver` (prompt text) + `testNlResolveLiveSmoke` (behaviour) |
| 9 | `testSayResolver` (absent / present / beat) |
| 10 | `testSayResolver` (subject **and** text argument, both ignored) |
| 11 | `testSpeakPrompt` (byte-identity across differing memory) |
| 12 | `testSpeakPrompt` (both sides of the cache boundary) |
| 13 | `testSpeakPrompt` (the id sweep, ids 317 / 419) |
| 14 | `testSpeakPrompt` (three prohibitions by substring) |
| 15 | `testSayConversation` (one Speak request) + `testSayInvariants` (no narrate) |
| 16 | `testAiRoleModel` |
| 17 | `testSayConversation` (first contact) |
| 18 | `testSayConversation` (second conversation, the latch) |
| 19 | `testSpeakRequestBody` (19 vs 20 lines) |
| 19a | `testNpcMemoryStampCoversPrevious` (helper) + `testSayConversation` (end to end) |
| 19b | `testSpeakRequestBody` (major with its profile row deleted) |
| 20 | `testValidateSpeech` (`profile: 42`) |
| 21 | `testValidateSpeech` (`summary: {}`) |
| 22 | `testSayConversation` (eight cases, one output) |
| 23 | `testSayConversation` (the detail is `kNoReply`, the constant) |
| 24 | `testSayConversation` (trigger injection + control arm) |
| 25 | **mutation check**, run by hand — red at `tests.cpp:12860`, then restored |
| 26 | `testSayConversation` (failed first contact, then asked again) |
| 27 | `testSayRenderBranches` |
| 28 | `testSayRenderBranches` + `testSayConversation` (byte-exact reply) |
| 29 | `testExamineGoldenSession`, literal untouched |
| 30 | `testSayRefusals` (exact output = line + `composeBand`) |
| 31 | `testSayInvariants` |
| 32 | `testSayInvariants` (counts + a `health` row checksum) |
| 33 | whole suite, keyless |
| 34 | the four authorised exceptions above, and nothing else |

Nothing is unmapped. REQ-NPCTALK-36 and -38 are stated, not built, as the plan
says; REQ-NPCTALK-37 is carried by `testNpcStoreInvariants`.

## Open items — all follow-ups, nothing blocking

1. **The explicit-`say` prefix leaks into the `said` row.** Described above.
   Needs a spec decision, not a code change.
2. **`testArchitectLiveSmoke` is broken against `seed/base.sql`** — it walks
   `east` from a room that has only a `north` exit, so its chain can never
   start. Not this brick's; found by running the gated suite.
3. **`validateAndLower`'s latent throw** (`src/nlresolve.cpp:276`), found while
   writing Step 7: `json::value("type", "")` raises `type_error.302` on a
   non-string field. Today it degrades to the parser because `aiResolve` wraps
   it, so it is a rough edge rather than a defect. `validateSpeech` uses an
   explicit `fieldEquals` instead.
4. **Streaming.** The latency table above is the evidence the spec's out-of-scope
   note asked for.
