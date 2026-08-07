---
title: "Implementation plan: npc-conversation"
date: 2026-08-07
status: executed
tags: [plan, npc, dialogue, say, isa, resolver, prompt-caching, tool-use, fallback]
modules: [npc, nlresolve, systems, loop, render, mutations, aihttp, parser, action]
related: [.lore/work/specs/npc-conversation.md, .lore/work/design/npc-conversation.md, .lore/work/specs/npc-memory-store.md, .lore/work/plans/npc-memory-store.md, .lore/work/specs/examine-perception-verb.md]
---

# Implementation plan: npc-conversation

Source of truth: [the spec](../specs/npc-conversation.md) (38 requirements
REQ-NPCTALK-1..38, 34 numbered validation items plus lettered sub-items) over
[the design](../design/npc-conversation.md) (9 decisions).

The [memory store](npc-memory-store.md) is **done and in the working tree** —
`writeCatalogProfile`, `writeNpcMemory`, `npcProfile`, `npcMemory`,
`npcLinesSince`, `kind='major'`, schema version 7. This plan calls those
helpers; it does not re-derive them. It changes exactly one line of one of them
(Step 2), for a reason the spec itself names.

## Guiding constraints

1. **Smallest atomic steps, skeleton first.** Step 1 widens the ISA so
   everything after it compiles; Step 4 makes `say` playable end to end with the
   model call absent; Steps 5–8 fill the call in. Each step is independently
   green.
2. **Every step ends at a validation gate** that a build + suite run can decide.
3. **The riskiest change goes last but one.** Step 9 edits the resolver prompt
   that governs the twelve shipping verbs. Everything else is green before it is
   touched, so a regression there is unambiguously attributable.
4. **Writes only through `mutations.cpp`.** The new translation unit contains no
   `INSERT`/`UPDATE`/`DELETE` — spec check 31 greps for exactly this.
5. **Offline by default.** No new test touches the network. The one live
   addition (Step 10) sits behind the existing `TEXTWORLD_AI_LIVE_TEST=1` gate.
6. **No AI-failure path may cost the turn; a DB fault must.** That distinction
   is a structural property of where the `try` block sits, and Step 8 states it
   as such rather than as discipline.

## Seams this touches (verified in tree)

| Seam | Where it is now | What this brick does |
|---|---|---|
| `Verb` enum | `src/action.hpp:11-12`, twelve verbs | add `Say` → thirteen |
| `Action` struct | `src/action.hpp:14-22`, `subject`/`direction`/`spell` | add `text` (engine-set only) |
| Fixed-verb parser | `src/parser.cpp:36-120` | add the `say` clause |
| `AiRole` | `src/aihttp.hpp:22`, four roles | add `Speak` → five |
| `modelForRole` / `roleName` | `src/aihttp.cpp` | `Speak` → prose model |
| Resolve tool enum | `src/nlresolve.cpp:219-221` | add `"say"` |
| `verbFromWord` | `src/nlresolve.cpp:112-126` | add `say` |
| Resolve scope payload | `src/nlresolve.cpp:181-192` | add `present_character` |
| Resolve system prompt | `src/nlresolve.cpp:148-172` | thirteenth verb + the conditional rule |
| Verb dispatch | `src/systems.cpp:209-259` `resolveImpl` | `case Verb::Say` |
| Narration dispatch | `src/loop.cpp:140-146` | skip AI narrate on a `Say` turn |
| Template renderer | `src/render.cpp:109-221` | `spoke` and `said` branches |
| `writeNpcMemory` | `src/mutations.cpp:783-799` | stamp the **previous** turn |
| Build | `CMakeLists.txt:17` `twcore` | `src/npc.cpp` |
| New unit | — | `src/npc.hpp`, `src/npc.cpp` |

Three helpers this brick consumes unchanged: `hostileInRoom`
(`src/combat.hpp:61`), `appendEvent` (`src/mutations.hpp:34`), and the three
`npc*` reads (`src/mutations.hpp:355-375`).

## Micro-decisions pinned before drafting

**1. `writeNpcMemory` stamps `currentTurn - 1`, not `currentTurn`.** REQ-NPCTALK-29a
requires the summary to be stamped with the turn *before* the current one, and
calls it "the one place where an off-by-one silently loses a conversation." The
shipped helper stamps `currentTurn(db)` (`mutations.cpp:797`), which would make
this turn's `said`/`spoke` invisible to every future `npcLinesSince`. The fix is
one line **inside** the helper — not a new parameter — because REQ-NPCSTORE-8's
reason for reading the turn internally still holds: no caller should get to
choose the stamp. The rule is unconditional and always true for this design: a
summary is composed by the model from lines it was handed, in the same call that
produces this turn's reply, so it can never cover this turn. Cost: three
existing `testNpcStore*` assertions move (Step 2). The alternative — a
`coversToTurn` argument — reopens exactly the hazard REQ-NPCSTORE-8 closed.

**2. The spoken text rides `Action::text`, an engine-only field.** REQ-NPCTALK-5
says `Say` carries `subject`, but the two input paths produce *different*
strings: the parser takes the remainder after `say ` (REQ-NPCTALK-8), the AI
resolver uses the whole raw line (REQ-NPCTALK-6). Neither fits `subject`.
REQ-NPCTALK-6 forbids the text **on the wire**, not in the engine's own struct —
so a fourth `Action` member, written only by `parse()` and `aiResolve()` and
never read out of a model response, satisfies it exactly. The `emit_action`
schema gains no text property, and `validateAndLower` never reads one.

**3. The parser slices the remainder from the raw line, not the lowered copy.**
`parse()` builds `lowered = trim(toLower(line))` and splits that. `toLower` is
byte-length preserving and `trim` cuts the same positions on both, so the same
split offset applied to `trim(line)` yields the player's bytes with casing
intact. Without this, `say Hello There` stores `hello there` and validation item
10's byte-equality fails.

**4. "No `Narrate` request" is asserted structurally plus at the resolve seam.**
`runTurn` binds production transports itself and has no injectable overload, so
nothing offline can count what it sent. Validation item 15 is therefore split:
the count (`exactly one Speak request`) is asserted through the injectable
`resolve(db, action, player, transport)` overload, and the absence of narration
is asserted as a source-text invariant over `src/loop.cpp` — the
`testBardOvertureContract` / `testNpcStoreInvariants` precedent, both of which
already pin behaviour that needs a live env to observe.

The structural half is **sufficient, not best-effort**: `aiRender` is the only
`AiRole::Narrate` call site in the binary, and it is called exactly once, at
`loop.cpp:142`, immediately inside the guard Step 11 pins by substring. Gating
that one call site gates every narrate request there is.

**5. Validation item 6 is a *live-gated* extension, not an offline test.** The
"existing phrasing tests" it names live in `testNlResolveLiveSmoke`
(`tests.cpp:4801`), behind `TEXTWORLD_AI_LIVE_TEST=1`; offline resolver tests
drive canned tool calls and cannot judge classification. So item 6 extends the
live smoke (Step 10), and its offline half asserts what offline *can* decide:
the payload carries `present_character`, and the prompt states the ordering rule
by substring. The spec's "offline throughout" preamble holds for the default
suite, which is what it is protecting.

**6. The field requests go in the user message, not the system block.**
REQ-NPCTALK-18 says the engine asks by varying the prompt; whether a profile is
wanted varies per call for the *same* character, so putting the ask in the
system block would break REQ-NPCTALK-21's byte-identity. The tool schema carries
all three properties unconditionally (stable), the user message carries the ask
(volatile), and the orchestration reads a field only when it asked for it — which
is what makes "unrequested is ignored" true by construction rather than by a
check.

**7. The three `npc*` read helpers stay in `mutations.cpp`.** Its comment invites
the conversation brick to hoist them. Don't: nine `testNpcStore*` tests and the
directory-walk invariant at `tests.cpp` (which special-cases `mutations.cpp`)
are written around their current home, and moving them buys nothing this brick
needs.

**8. Validation check 32 ("no component-table row") is asserted on a talk turn
with no hostile present.** With a hostile the turn is refused, but `resolveCombat`
still runs after `resolve` in the same tick (`loop.cpp:126`) and the enemy's turn
writes `health`. That is combat behaving normally, not the talk turn writing;
the check is only coherent on the successful shape.

**9. The tool is named `emit_reply`**, following `emit_action` and `create_room`.
The design calls it "the `reply` tool" after its required field; the house
naming pattern wins.

**10. A spec cross-reference to read past.** REQ-NPCTALK-10 cites
"REQ-NPCTALK-19's no-reply refusal", but REQ-NPCTALK-19 is the fold threshold.
The no-reply refusal is REQ-NPCTALK-31. Read it as -31 throughout.

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.6; padding: 8px 0;">
<b>1</b> ISA widening: Verb::Say, Action::text, AiRole::Speak<br>
&nbsp;&nbsp;│<br>
&nbsp;&nbsp;├─▶ <b>2</b> writeNpcMemory stamps turn − 1&nbsp;&nbsp;<i>(independent; needed by 8)</i><br>
&nbsp;&nbsp;│<br>
&nbsp;&nbsp;└─▶ <b>3</b> parser: <code>say &lt;text&gt;</code><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▼<br>
&nbsp;&nbsp;&nbsp;&nbsp;<b>4</b> new TU + refusals + dispatch + render + narrate skip &nbsp;<i>← playable offline</i><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>5</b> the prompt (system block + user message) ──▶ <b>6</b> request body + emit_reply<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▼<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>7</b> validateSpeech (the 8 failures)<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└──────────────── (2) ────────────────────────▶ <b>8</b> orchestration + the writes<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▼<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>9</b> the resolver &nbsp;<span style="background:#fdecea;color:#b3261e;padding:1px 6px;border-radius:3px;">HIGH</span><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>10</b> live smoke (gated)<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>11</b> whole-spec gates ──▶ <b>12</b> final validation<br>
</div>

Risk legend:
<span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified ·
<span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> touches shipped behaviour, regression-guarded ·
<span style="background:#fdecea;color:#b3261e;padding:1px 6px;border-radius:3px;">HIGH</span> prompt-tuning territory, failure is quiet.

Step 4 is the pivot: after it, `say hello` is a complete playable turn offline
(refusal or no-reply line), and Steps 5–8 replace the missing answer without
changing any of the surrounding shape. Step 2 is independent of 1/3/4 and can
land at any point before Step 8; it is placed second because it is the one
change to already-shipped behaviour, and doing it while the tree is otherwise
untouched makes its three test moves unambiguous.

---

### Step 1 — ISA widening: `Verb::Say`, `Action::text`, `AiRole::Speak`
**Requirements:** REQ-NPCTALK-5, -16. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Skeleton first: this step makes the types exist and nothing behave. Nothing
emits `Verb::Say` yet, so the only observable change is that the enum has
thirteen members and `AiRole` five.

In **`src/action.hpp:11-12`**, add `Say` at the end of `Verb`. In the struct,
add the fourth member with the comment that carries micro-decision 2:

```cpp
enum class Verb { Look, Go, Take, Drop, Inventory, Wait, Quit, Attack, Cast, Read,
                  Spells, Examine, Say };

struct Action {
    Verb verb;
    int64_t subject = 0;   // entity id for Take/Drop/Examine; target enemy for Attack
                           // (0 = the hostile in the room); 0 when unused. For Say
                           // it stays 0: resolution finds the character in the room.
    std::string direction; // for Go; empty when unused
    std::string spell;     // for Cast: the catalogued spell key; empty otherwise
    // For Say: the player's own words, ENGINE-SET AND NEVER MODEL-SET
    // (REQ-NPCTALK-6). parse() puts the remainder of the line here; aiResolve
    // puts the whole raw line here. No model response is ever read into it, and
    // the emit_action schema has no text property, so a model cannot paraphrase
    // what the player said. Empty for every other verb.
    std::string text;
};
```

In **`src/aihttp.hpp:22`**, `enum class AiRole { Resolve, Narrate, Generate, Bard, Speak };`
appended, with a comment naming why dialogue gets its own role: it occupies the
slot narration would have used on a talk turn, so a profile log can tell the two
apart. In **`src/aihttp.cpp`**, `roleName` gains `case AiRole::Speak: return "speak";`
and `modelForRole` gains `Speak` → the prose model default, under the unchanged
`TEXTWORLD_MODEL` precedence.

Existing `switch (action.verb)` sites that must gain a `Say` arm to compile
without a warning: `resolveImpl` (`src/systems.cpp:211`). It gets a
`throw std::logic_error` placeholder in this step, replaced in Step 4 — the
`Verb::Quit` precedent at `systems.cpp:246-251`, so an unrouted verb surfaces
immediately rather than silently costing a turn.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>cmake --build build</code> clean with no new
warnings; full suite green and <b>unchanged in count except</b> the role tests.
Extend <code>tests.cpp:3689-3717</code>: <code>roleName(AiRole::Speak) == "speak"</code>;
<code>modelForRole(AiRole::Speak) == "claude-opus-4-8"</code> by default and
<code>"claude-sonnet-5"</code> under <code>TEXTWORLD_MODEL</code> (spec check 16).
Add a compile-time-ish guard that the ISA is thirteen: a <code>switch</code> over
every <code>Verb</code> in a new <code>testSayIsaShape</code> with no
<code>default:</code>, so a fourteenth verb added later fails to compile here.
</blockquote>

### Step 2 — `writeNpcMemory` stamps the previous turn
**Requirements:** REQ-NPCTALK-29a. **Size:** S · **Token-risk:** <span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> *(edits shipped, tested behaviour)*

One line in **`src/mutations.cpp:797`**:

```cpp
    // The stamp is the turn the summary COVERS TO, and that is always the turn
    // BEFORE this one (REQ-NPCTALK-29a). The summary is written by the model
    // from the lines it was handed, in the same call that produces this turn's
    // reply — so it cannot cover this turn's own exchange, and stamping the
    // current turn would make that exchange invisible to every future
    // npcLinesSince read. Still read here rather than passed in
    // (REQ-NPCSTORE-8): no caller gets to choose the stamp.
    const int64_t covers = currentTurn(db) - 1;
    s.bind(3, covers < 0 ? 0 : covers);
```

Amend the header comment at **`src/mutations.hpp:328-337`** to say "the turn
BEFORE the current one" and carry the reason. Amend
[the memory-store spec](../specs/npc-memory-store.md) REQ-NPCSTORE-8 with a
one-line note that the conversation brick pinned the stamp to `turn - 1` and
why — the spec should not keep describing behaviour the code no longer has.

Three existing assertions move, all in `tests/tests.cpp`:

- `testNpcStoreMemory:10921` — `summary_turn` after writing at `meta.turn = 4`
  becomes `3`.
- `testNpcStoreMemory:10932,10938` — after writing at `meta.turn = 9`, `8`.
  The comment at `:10924-10926` ("the stamp moves with meta.turn") stays true
  and stays.
- `testNpcStoreLines:10992-10998` — the summary written at turn 2 now stamps 1,
  so the turn-2 `said` ("what is behind the gate") is no longer filtered out.
  Advance `meta.turn` to 3 before the `writeNpcMemory` call so the fixture still
  means "everything up to and including the third line is summarised", keeping
  the assertion's *intent* rather than editing its expectation.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> full suite green. Add <code>testNpcMemoryStampCoversPrevious</code>
written to <b>fail</b> under the old behaviour (validation item 19a in its
helper-level form): at <code>meta.turn = 7</code>, append <code>said</code> +
<code>spoke</code>, call <code>writeNpcMemory</code>, then assert
<code>npcLinesSince</code> <b>still returns both rows</b> and
<code>npcMemory(...).summaryTurn == 6</code>. Confirm by hand that reverting the
one line turns this test red, then restore — a guard never seen to fail is not a
guard (the Step 11 precedent from the memory-store brick).
</blockquote>

### Step 3 — The parser: `say <text>`
**Requirements:** REQ-NPCTALK-8, -9, -10. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

In **`src/parser.cpp`**, before the bare-spell-word fallback, add the clause.
Note the second `trim(line)` — micro-decision 3:

```cpp
    // Speech (REQ-NPCTALK-8): the remainder of the line is the spoken text,
    // taken VERBATIM from the raw input rather than from `lowered`, so the
    // player's casing and punctuation survive into the `said` row. toLower is
    // byte-length preserving and trim cuts the same positions on both, so the
    // split offset computed above applies unchanged. The target is NOT resolved
    // here (REQ-NPCTALK-9): subject stays 0 and resolution finds the character
    // in the room, the shape `attack` already uses.
    if (verbWord == "say") {
        if (arg.empty()) return std::nullopt;  // bare verb, REQ-PROTO-6a
        Action a{Verb::Say};
        a.text = trim(trim(line).substr(split));
        return a;
    }
```

Nothing about AI availability is consulted (REQ-NPCTALK-10): the parser is
reached either way, and resolution decides what a `say` with no reachable model
produces.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testParseSay</code> beside the existing parser
tests: <code>say hello</code> → <code>{Say, subject 0, text "hello"}</code>;
<code>say Hello There, Warden!</code> → text byte-equal to
<code>"Hello There, Warden!"</code> (the casing guard — this fails if the lowered
copy is sliced); <code>say   spaced   out  </code> → text
<code>"spaced   out"</code> (outer trim only, interior preserved); bare
<code>say</code> and <code>say&nbsp;&nbsp;&nbsp;</code> → <code>nullopt</code>;
<code>sayonara</code> → <code>nullopt</code>, not a <code>Say</code> with text
<code>""</code>. Full suite otherwise unchanged.
</blockquote>

### Step 4 — The new unit: target, refusals, dispatch, rendering, narrate skip
**Requirements:** REQ-NPCTALK-1, -2, -3, -4, -4a, -7, -17 (half), -26, -27, -28, -31 (the AI-disabled case). **Size:** L · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

After this step `say` is a complete playable turn with no model call anywhere.

**`src/npc.hpp`** — the new unit's contract. Declares:

```cpp
// The character in `room`, or 0 if there is none: an entity whose catalog row
// has kind IN ('character','major') (REQ-NPCTALK-2). Beats are objects — a
// scorched lectern is examinable, not talkable. At most one can exist, by rules
// already set (REQ-NPCTALK-1), so this returns an id rather than a list; the
// day that rule breaks, this signature is what breaks with it.
int64_t characterInRoom(Db& db, int64_t room);

// The three engine-authored lines. Exposed so tests assert the EXACT string,
// and so a failure can never surface as fabricated dialogue (REQ-NPCTALK-31).
extern const char* const kNoOneToTalkTo;
extern const char* const kNoTalkingInCombat;
extern const char* const kNoReply;

// Unsummarised lines at which the engine asks for a memory fold
// (REQ-NPCTALK-19). Under mutations.hpp's kLineCap of 40, so the bounded read
// never truncates in practice.
inline constexpr size_t kFoldThreshold = 20;

void resolveSay(Db& db, int64_t player, const std::string& text);
void resolveSay(Db& db, int64_t player, const std::string& text,
                const HttpTransport& transport);
```

**`src/npc.cpp`** — this step writes the refusal skeleton only:

```cpp
void resolveSayImpl(Db& db, int64_t player, const std::string& text,
                    const HttpTransport* transport) {
    const int64_t room = roomOf(db, player);
    const int64_t character = characterInRoom(db, room);

    // REQ-NPCTALK-4a: the no-one-here check runs FIRST. An empty room that
    // holds a hostile yields this line, not the combat one — with nobody
    // present, "you can't talk during a fight" would imply there was someone
    // to talk to.
    if (character == 0) {
        appendEvent(db, player, "failed", 0, 0, kNoOneToTalkTo);
        return;
    }
    if (hostileInRoom(db, room) != 0) {
        appendEvent(db, player, "failed", 0, 0, kNoTalkingInCombat);
        return;  // no model call is issued on this path (validation item 2)
    }
    // Step 8 replaces this with the call. Until then every reachable
    // conversation takes REQ-NPCTALK-31's no-reply shape, which is exactly what
    // an AI-disabled run does permanently.
    appendEvent(db, player, "said", character, 0, text.c_str());
    appendEvent(db, player, "failed", 0, 0, kNoReply);
}
```

`characterInRoom` is one SELECT joining `location` to `catalog` on
`catalog.entity`, filtered `kind IN ('character','major')`, `ORDER BY entity
LIMIT 1`. `npc.cpp` also needs its **own** file-local `roomOf` — that lookup is
independently reimplemented in seven units already (`band.cpp`, `prose.cpp`,
`combat.cpp`, `nlresolve.cpp`, `systems.cpp`, `loop.cpp`, `render.cpp`) under
the house "reimplement, don't reach across TUs" rule, and this unit is the
eighth, not an exception to it.

**`src/systems.cpp`** — replace Step 1's placeholder in `resolveImpl`, mirroring
the `resolveGo` transport shape (`systems.cpp:120-125`):

```cpp
        case Verb::Say:
            transport != nullptr
                ? resolveSay(db, player, action.text, *transport)
                : resolveSay(db, player, action.text);
            break;
```

**`src/loop.cpp:140-146`** — the narrate skip (REQ-NPCTALK-27). The reply *is*
the AI output; there is no second call to run alongside it:

```cpp
    // No AI narration on a talk turn (REQ-NPCTALK-27): the character's reply is
    // the AI output and prints verbatim, so a narrator here would paraphrase
    // the one thing this feature refuses to paraphrase. Refusals take the same
    // branch — a talk turn has exactly one shape, and the template renderer
    // prints the `failed` detail as it does everywhere else.
    const ScopedStage narrateStage("narrate");
    if (action->verb != Verb::Say && aiNarrationEnabled()) {
```

**`src/render.cpp`** — two branches after `examined` (REQ-NPCTALK-26):

```cpp
        } else if (verb == "spoke") {
            // The character's reply, VERBATIM (REQ-NPCTALK-25) — the treatment
            // canon room descriptions already get. Nothing wraps or rewords it.
            out += detail + "\n";
        } else if (verb == "said") {
            // Deliberately nothing: the player already saw what they typed.
            // Present as an explicit branch rather than falling through to the
            // unrecognised-verb default, so the silence is a decision on the
            // page instead of an accident (REQ-NPCTALK-26).
```

**`CMakeLists.txt:17`** — `src/npc.cpp` into `twcore`, placed after
`src/mutations.cpp` so the list still reads roughly bottom-up.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> clean build; full suite green including
<code>testExamineGoldenSession</code> <b>unmodified</b> — that literal is the
byte-identity proof for the other twelve verbs (spec check 29), and it runs
AI-disabled, so it exercises the new <code>loop.cpp</code> branch on every turn.
New <code>testSayRefusals</code> driving <code>runTurn</code> against
<code>combat_fixture.sql</code>: (a) no character present → output is exactly
<code>kNoOneToTalkTo</code> + band, <code>meta.turn</code> +1 (item 1); (b)
character + hostile → exactly <code>kNoTalkingInCombat</code>, turn +1 (item 2);
(c) a room holding only a <code>beat</code> catalog entity → the no-one-here line
(item 3); (d) no character but a hostile present → the <b>no-one-here</b> line
(item 3a); (e) the <code>said</code> row's <code>turn</code> column equals the
incremented <code>meta.turn</code> (item 3b, which is what makes REQ-NPCTALK-7
observable). Plus <code>testSayRenderBranches</code>: a hand-written
<code>said</code> + <code>spoke</code> pair in one turn renders as the
<code>spoke</code> detail byte-exact and nothing else, including a detail with
internal newlines and punctuation (item 27, item 28's template half).
</blockquote>

### Step 5 — The prompt: a stable system block and a volatile user message
**Requirements:** REQ-NPCTALK-20, -21, -22, -23, -24. **Size:** M · **Token-risk:** <span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span>

Two pure builders in `npc.cpp`, declared in `npc.hpp` so both halves are
directly testable without a transport.

`kSpeakRulesPrompt` — a git-versioned `const char* const`, prepended to every
composed system block (REQ-NPCTALK-22). It states the three prohibitions
verbatim so each is spot-checkable by substring (REQ-NPCTALK-23, validation item
14): never explain a mechanic — a weakness, damage, a cooldown, a resistance;
never volunteer background unprompted; never name a place, person, or object not
already established. It also carries the reason these live here and not in a
character file: a rule a player will actively attack cannot live in a file an
author can edit or forget.

```cpp
// The system block: engine rules, then this character's profile, then the
// setting. All three are STABLE — the block is byte-identical across two
// conversations with the same character whose memory differs (REQ-NPCTALK-21),
// which is the cache prefix and is asserted as a property, not intended as one.
// Composed at runtime rather than being a constant, because the profile is per
// character — the one departure from kArchitectPrompt / kResolveSystemPrompt,
// which is why the engine-owned half stays a constant that is prepended.
std::string buildSpeakSystem(Db& db, int64_t character);

// The user message: memory summary, recent lines, the player's line, and the
// engine's asks. VOLATILE, all of it. The asks live here rather than in the
// system block because whether a profile is wanted varies per call for the same
// character, and putting them above would break byte-identity.
struct SpeakAsks { bool profile = false; bool summary = false; };
std::string buildSpeakUser(Db& db, int64_t character, const std::string& line,
                           SpeakAsks asks);
```

`buildSpeakSystem` reads `npcProfile(db, character)` and `meta.setting`.
`buildSpeakUser` reads `npcMemory(db, character).summary` and
`npcLinesSince(db, character)`, emitting each line as speaker + detail.

**The shield (REQ-NPCTALK-24).** Only `said`/`spoke` details reach the payload,
and those are free text by construction — no entity id, catalog id, tier,
`seeded` flag, handle, or `archetype|element` tag is read anywhere in either
builder. This is the same posture `prose.cpp` takes when it withholds the
`burned`/`froze`/`materialized` details from the narrator; here it falls out of
which columns are selected rather than needing a filter.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testSpeakPrompt</code>. <b>Item 11:</b> build
the system block for one character, write a memory summary and three
<code>said</code>/<code>spoke</code> rows, build it again — assert
<code>==</code> on the two strings. <b>Item 12:</b> the system block contains the
rules constant, the profile text, and <code>meta.setting</code>; the user message
contains the summary text, each line's detail, and the input line — and the
system block contains <b>none</b> of the three volatile strings. <b>Item 14:</b>
<code>contains(kSpeakRulesPrompt, ...)</code> for one distinctive phrase from
each of the three prohibitions. <b>Item 13:</b> a sweep over
<code>buildSpeakSystem + buildSpeakUser</code> for a world whose character has
catalog id 317, <b>entity id 419</b>, tier 3, <code>seeded = 1</code> and handle
<code>scorched_lectern</code> — assert none of <code>"317"</code>,
<code>"419"</code>, <code>"tier"</code>, <code>"seeded"</code>,
<code>"scorched_lectern"</code> appears. <b>Entity id is the first thing
REQ-NPCTALK-24 names and is the easiest to leak</b>, since every helper in this
unit takes one as an argument; the ids are in the hundreds so the probes cannot
collide with the fixture's prose.
</blockquote>

### Step 6 — Request body and the `emit_reply` tool
**Requirements:** REQ-NPCTALK-18, -18a, -19. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`std::string buildSpeakRequestBody(const std::string& system, const std::string& user);`
in `npc.hpp`, modelled on `buildResolveRequestBody` (`nlresolve.cpp:199-250`):
`model = modelForRole(AiRole::Speak)`, `max_tokens` sized for a reply plus an
optional profile and summary, `system`, one user message, one tool, and
`tool_choice` forcing the call (unlike the resolver's `auto` — a talk turn always
wants a reply; "no tool call" is a failure here, not a designed path). No
thinking, no stream, no cache-control key.

The tool carries all three properties unconditionally; only `reply` is required:

| Property | Type | Required | Read by the engine when |
|---|---|---|---|
| `reply` | string | yes | always |
| `profile` | string | no | `npcProfile(db, character).empty()` |
| `summary` | string | no | `npcLinesSince(db, character).size() >= kFoldThreshold` |

The two conditions are computed once in Step 8 and passed down as `SpeakAsks`,
so the ask in the prompt and the field the engine reads are the same bit — an
unrequested field is ignored because nothing looks at it (REQ-NPCTALK-18).

The profile condition is stated over the **row**, never over `kind`
(REQ-NPCTALK-18a): `npcProfile` returns empty both for a minor character on
first contact and for a major whose profile row is missing, and both take the
same branch rather than one being a special case that has to be discovered.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testSpeakRequestBody</code>: the exact
top-level key set (the shape <code>tests.cpp</code> already pins for the resolver
body), the tool named <code>emit_reply</code> with exactly three properties and
<code>required == ["reply"]</code>, <code>model</code> honouring
<code>TEXTWORLD_MODEL</code>, and no <code>thinking</code>/<code>stream</code>/
<code>cache_control</code> key anywhere. <b>Item 19:</b> a fixture with 19
unsummarised lines produces a user message with no summary ask; at 20 it does.
<b>Item 19b:</b> a <code>major</code> with a profile row is not asked; the same
major with its <code>catalog_profile</code> row deleted <b>is</b> asked, through
the same branch — assert by driving <code>buildSpeakUser</code>, not by reading
<code>kind</code>.
</blockquote>

### Step 7 — `validateSpeech`: one gate, eight failure cases
**Requirements:** REQ-NPCTALK-31, -32. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

```cpp
struct SpeechReply {
    std::string reply;    // never empty in a returned value
    std::string profile;  // "" = absent or dropped
    std::string summary;  // "" = absent or dropped
};

// Pure function of the response. SELECTs nothing, never throws — the
// validateAndLower / validateAiResponse shape. Returns nullopt iff the response
// cannot yield a reply; each rejection emits ONE diagnostic naming the clause.
std::optional<SpeechReply> validateSpeech(const HttpResponse& response);
```

Clauses, in order, covering five of REQ-NPCTALK-31's eight cases (the other
three — AI disabled, transport error, timeout — are decided before or at the
transport and are handled in Step 8):

- **a.** `status == 200`. A transport error carries status 0 and fails here too.
- **b.** the body parses as a JSON object with a `content` array.
- **c.** exactly one `tool_use` block named `emit_reply`; zero is a failure here
  (unlike the resolver, where zero is the designed no-action path).
- **d.** `input.reply` present, a string, and non-empty after trim.

`profile` and `summary` are **never** clauses (REQ-NPCTALK-32): a missing or
non-string field is dropped to `""` and the reply still lands. A bad part never
costs the whole turn — the leniency materialisation established.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testValidateSpeech</code>, one case per
clause: status 500, transport error (status 0), a body that is not JSON, a
well-formed body with no tool call, a tool call with no <code>reply</code>, a
tool call with <code>reply: ""</code>, and <code>reply</code> present but not a
string — all <code>nullopt</code>. Then the leniency half (<b>items 20, 21</b>):
a valid reply alongside <code>profile: 42</code> and alongside
<code>summary: {}</code> each yields the reply with that field empty. Assert the
function never throws by driving a deliberately hostile body (deeply nested,
wrong types throughout).
</blockquote>

### Step 8 — Orchestration: one call, the writes, and where the `try` sits
**Requirements:** REQ-NPCTALK-6, -17, -25, -29, -29a, -30, -31, -33, -34, -35. **Size:** L · **Token-risk:** <span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span>

Replace Step 4's no-reply stub. The structure below is the requirement, not an
illustration of it — REQ-NPCTALK-33's rollback-versus-degrade distinction is
**where the `try` block ends**, and nothing else enforces it:

```cpp
    // Reads BEFORE the call: the two asks, and the lines the prompt carries.
    const SpeakAsks asks{npcProfile(db, character).empty(),
                         npcLinesSince(db, character).size() >= kFoldThreshold};

    std::optional<SpeechReply> got;
    if (aiNarrationEnabled()) {          // REQ-NPCTALK-31 case 1, checked first
        try {                            // ── AI failure lives INSIDE here ──
            const std::string body = buildSpeakRequestBody(
                buildSpeakSystem(db, character),
                buildSpeakUser(db, character, text, asks));
            const HttpResponse resp = transport != nullptr
                ? (*transport)(body)
                : makeAnthropicTransport(AiRole::Speak)(body);
            got = validateSpeech(resp);  // EXACTLY ONE call, no retry, ever
        } catch (const std::exception& e) {
            logEmitf(LogLevel::Warn, "npc", "resolveSay: no reply: %s", e.what());
        } catch (...) {
            logEmit(LogLevel::Warn, "npc", "resolveSay: no reply: unknown");
        }
    }
    // ── and the writes are OUTSIDE it (REQ-NPCTALK-33) ──────────────────────
    // A database fault here propagates to runTurn, which rolls the tick back:
    // no said row, no spoke row, meta.turn unchanged. It must NEVER degrade to
    // the no-reply line, because that would report a world that did not change
    // as one that did. Widening the catch above to cover these calls is the
    // exact downgrade the spec forbids, and Step 11's mutation check proves the
    // suite notices.
    appendEvent(db, player, "said", character, 0, text.c_str());
    if (!got) {
        appendEvent(db, player, "failed", 0, 0, kNoReply);
        return;                          // REQ-NPCTALK-34: no profile written
    }
    appendEvent(db, character, "spoke", player, 0, got->reply.c_str());
    if (asks.profile && !got->profile.empty()) {
        writeCatalogProfile(db, catalogOf(db, character), got->profile);
    }
    if (asks.summary && !got->summary.empty()) {
        writeNpcMemory(db, character, got->summary);   // stamps turn - 1
    }
```

Five properties this shape gives rather than asserts:

- **One model call per talk turn** (REQ-NPCTALK-17), no retry, and the narrator
  already skipped by Step 4's `loop.cpp` branch.
- **`said` before `spoke`** (REQ-NPCTALK-29), because `npcLinesSince` returns
  rows in log order and a reply preceding its question would read back as one.
- **The player's words come from `text`** (REQ-NPCTALK-6), which came from the
  engine, never from `got`.
- **A failed first contact writes no profile** (REQ-NPCTALK-34) — the early
  return is before the write, so the next conversation asks again.
- **A second profile changes nothing** (REQ-NPCTALK-35), because
  `writeCatalogProfile` is a one-way latch; the engine does not re-check.

`catalogOf(db, entity)` is one more SELECT in `npc.cpp` (`SELECT id FROM catalog
WHERE entity = ?`) — `writeCatalogProfile` is keyed by catalog id, not entity.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testSayConversation</code> driving
<code>resolve(db, action, player, transport)</code> with fakes.
<b>Item 4:</b> a character present → one request issued, the reply reaches the
rendered turn byte-for-byte including punctuation and internal newlines (also
<b>item 28</b>). <b>Item 15:</b> the fake counts requests and sees exactly one,
whose body's model is the <code>Speak</code> role's. <b>Item 22:</b> all eight
failure cases — AI disabled, transport error, timeout, status 500, unparseable
body, no tool call, missing <code>reply</code>, empty <code>reply</code> — each
produce <code>said</code> + <code>failed</code>, <code>meta.turn</code> +1, and
<b>byte-identical output across all eight</b>, asserted as a set of one. Timeout
is driven as its own fake (transport error with the timeout shape), not folded
into the transport case, because the two arrive by different code.
<b>Item 23:</b> the fallback detail <code>== kNoReply</code>, the constant.
<b>Items 17, 18, 26:</b> first contact stores the profile and prints the reply;
the second conversation does not ask, and a profile supplied anyway leaves the
stored text unchanged; a failed first contact leaves no row and the next
conversation asks again. <b>Item 19a:</b> after a turn that writes a summary,
<code>npcLinesSince</code> still returns that turn's <code>said</code> and
<code>spoke</code> — the end-to-end form of Step 2's helper-level test.
<b>Item 24:</b> fault injection — make the <code>npc_memory</code> write fail
(a temporary trigger, or a read-only attach on that table) and assert full
rollback: no <code>said</code>, no <code>spoke</code>, <code>meta.turn</code>
unchanged; plus a <b>control arm</b> in which the injection is installed but the
turn writes no memory, proving it touches nothing else.
</blockquote>

### Step 9 — The resolver: `say` in the ISA, `present_character`, the conditional rule
**Requirements:** REQ-NPCTALK-11, -12, -13, -14, -15, -15a. **Size:** M · **Token-risk:** <span style="background:#fdecea;color:#b3261e;padding:1px 6px;border-radius:3px;">HIGH</span>

The spec names this the highest-risk change it contains, and the reason is that
its failure is quiet: `take the key` classified as speech reads as the character
ignoring you.

Four edits in **`src/nlresolve.cpp`**:

1. `verbFromWord` (`:112-126`) gains `if (word == "say") return Verb::Say;`, and
   the tool's `enum` array (`:219-221`) gains `"say"` — the two lists the suite
   asserts element-wise identical (REQ-NPCTALK-11). Update the "twelve" in both
   comments to thirteen — **and at `:153`, which is not a comment**: "The
   instruction set has exactly twelve verbs" is a sentence the model reads, and
   a stale count there is a wrong instruction, not a stale note. (`:170` says
   "these seven single actions" — a pre-existing stale numeral from when the ISA
   had seven verbs. The replacement rule below drops the numeral rather than
   correcting it, which is the durable fix.)
2. `validateAndLower`'s switch (`:310-367`): `case Verb::Say:` joins the
   argument-free group with `Look`/`Attack`/etc. (REQ-NPCTALK-13). A stray
   `subject` or text argument is ignored by falling into clause e, which is
   exactly what validation item 10 asks for. **`action.text` is not set here** —
   `aiResolve` sets it (below), because `validateAndLower` does not receive the
   line.
3. `aiResolve` (`:371-396`): after a successful lower, `if (action->verb ==
   Verb::Say) action->text = line;` — the whole raw line, the engine's own copy
   (REQ-NPCTALK-6).
4. `buildResolveContext` (`:174-197`): add `present_character` — the character's
   noun — **only when one exists** (REQ-NPCTALK-12). Written as
   `if (const int64_t c = characterInRoom(db, room)) payload["present_character"] = nameOf(db, c);`
   so the key is absent rather than present-and-empty. This is the one place
   `nlresolve.cpp` reaches into `npc.hpp`; it is a SELECT-only call and the
   unit's read-only contract holds.

The fifth edit is the prompt (`:148-172`), and it is the risk. Add `say` as the
thirteenth verb, and make the existing absolute rule conditional:

> If the input is a question, chatter, an unknown verb, or anything that is not
> one of these single actions, make no tool call at all. When in doubt, make no
> call. **This rule changes when `present_character` is supplied.** Then the
> player can speak to that character, and the ordering is: first, if the line
> clearly means one of the other twelve actions, emit that action; only if it
> does not, emit `say`. A question, a greeting, or chatter is speech. A line
> naming an item, a direction, or a spell is an action, not speech, even when it
> is phrased politely.

REQ-NPCTALK-15a is recorded in the code comment above the prompt, not just in
the spec: what is contractual is the named regression set below; wording outside
it is tuned against real play, and a passing suite is not a settled rule. Both
directions of misclassification are recoverable by retyping, which is why an
imprecise boundary is acceptable here and would not be for a rule that wrote to
the world.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <b>Item 5:</b> extend the existing element-wise
enum/<code>verbFromWord</code> equality assertion to thirteen — it should fail
before the second edit lands, which is worth observing once.
<b>Item 9:</b> a captured payload for a room with no character has <b>no</b>
<code>present_character</code> key (<code>!payload.contains(...)</code>, not
<code>== ""</code>); with one, the key holds that character's noun and no id.
<b>Item 10:</b> an <code>emit_action</code> response of
<code>{"verb":"say","subject":"warden","text":"paraphrased"}</code> lowers to
<code>Verb::Say</code>, and after <code>aiResolve</code> the resulting
<code>said</code> row's detail is the <b>player's raw line</b>, byte-equal —
the model's <code>text</code> reaching the row is the failure this asserts
against. <b>Items 7, 8:</b> offline, with the resolver prompt as the only input
that changed, assert the prompt <i>text</i> contains the conditional rule and
still contains the unconditional no-call sentence for the absent case — the
behavioural halves are item 6's job. Plus a stale-numeral guard:
<code>kResolveSystemPrompt</code> contains <code>"thirteen verbs"</code> and
contains neither <code>"twelve verbs"</code> nor <code>"seven single actions"</code>,
so the count the model reads cannot drift from the enum again. Full suite green, including every existing
resolver test <b>unmodified</b> except the two authorized above.
</blockquote>

### Step 10 — The live smoke, extended (gated)
**Requirements:** REQ-NPCTALK-15 (the behavioural half). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> *(no-op in the default run)*

Extend `testNlResolveLiveSmoke` (`tests.cpp:4801`), keeping its two standing
rules: mechanical invariants only, and a `nullopt` is a clean fallback rather
than a failure.

**The named regression set (validation item 6)** — a fixture room supplying
`present_character`, and each of these asserted to lower to its own verb and
never to `Say`: `take the key`, `go north`, `attack`, `cast ward`,
`read grimoire`, `examine candle`, `look`, `inventory`. Then the positive
direction: a greeting and a question resolve to `Say` (or cleanly decline).

This is the contract REQ-NPCTALK-15a names. It runs only under
`TEXTWORLD_AI_LIVE_TEST=1`, so the default suite stays offline and keyless — and
it is a **prompt-tuning loop by design**, which is precisely why it is fenced
off from the suite that gates the brick.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> default run — the new block is a no-op and the suite's
check count is unchanged. Gated run, executed once by hand with a key: the eight
regression phrasings hold. If any resolves to <code>Say</code>, tune the
prompt's ordering sentence and re-run — bounded to this fixed set, never opened
into an open-ended tuning session against invented phrasings.
</blockquote>

### Step 11 — The whole-spec gates, encoded as tests
**Requirements:** REQ-NPCTALK-24, -33, -36, -37. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`testSayInvariants`, following `testNpcStoreInvariants`:

- **Item 31:** no line of `src/npc.cpp` contains `INSERT`, `UPDATE`, or
  `DELETE`. Non-vacuous: assert the file is non-empty and calls `appendEvent`.
- **Micro-decision 4:** `src/loop.cpp` contains
  `action->verb != Verb::Say && aiNarrationEnabled()` verbatim — the structural
  half of item 15. Non-vacuous: assert the file also still contains `aiRender`.
- **Item 32:** a successful talk turn with no hostile present changes no
  component-table row — `COUNT(*)` over `location`, `health`, `known_spells`,
  `hostile`, and `exits` before and after, plus a full-row checksum on `health`
  so a value change with a stable count is caught. This is the mechanical form
  of the social-engineering defence, and it is why the defence is structural
  rather than a prompt rule.
- **REQ-NPCTALK-37:** the bard's wake predicate still reads
  `verb IN ('generated','defeated','learned','materialized')` and `src/bard.cpp`
  mentions neither speech verb. Already asserted by `testNpcStoreInvariants`;
  extend its comment to name this brick, since this is the brick that made the
  verbs reachable.
- **REQ-NPCTALK-36** is a statement, not work: nothing here acts on the bard
  reading conversations. Recorded in the coverage map, built nowhere.

**Mutation check (item 25),** run by hand and reverted, the memory-store Step 11
protocol: widen Step 8's `catch` to cover the `appendEvent`/`writeNpcMemory`
calls — the exact downgrade REQ-NPCTALK-33 forbids — and confirm the suite turns
red at item 24's fault-injection test. Restore and re-verify green. A guard that
has never been seen to fail is not a guard.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> full suite green; the mutation check observed red, then
reverted and observed green again. Record the red line number in the
implementation notes, as the memory-store brick did.
</blockquote>

### Step 12 — Final validation against the spec
**Requirements:** all 38, re-read. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

1. `cmake --build build --clean-first`, whole suite green, **no network and no
   `ANTHROPIC_API_KEY`** (item 33).
2. Every existing resolver, narrator, architect, bard, and combat test passes
   **unmodified** (item 34). The authorized exceptions are enumerated, not
   waved past: the role tests (Step 1), three `testNpcStore*` assertions
   (Step 2), and the resolver enum-equality assertion (Step 9). Any fourth is a
   divergence and goes in the notes.
3. `testExamineGoldenSession` passes with its literal **untouched** (item 29).
4. `grep -En "INSERT|UPDATE|DELETE" src/npc.cpp` — empty (item 31).
5. Walk the 34 validation items against the tests that carry them, using the
   coverage map below; anything unmapped is a gap, not a judgement call.
6. Play the game against a fresh `world.db` with a key set: reach a room with a
   character, hold a short conversation, confirm the reply prints verbatim with
   the status band, confirm `catalog_profile` gained exactly one row, and quit
   cleanly. The first real end-to-end run is the only place latency is felt —
   note the observed wait, since streaming is the deferred lever
   ([out of scope](../specs/npc-conversation.md#out-of-scope)) and this is the
   evidence for when to take it.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> all six above, recorded in
<code>.lore/work/notes/npc-conversation.md</code> with any divergence written
down rather than absorbed.
</blockquote>

## Coverage map

| Requirement | Step | Validation item |
|---|---|---|
| REQ-NPCTALK-1, -2 | 4 | 3 |
| REQ-NPCTALK-3, -4, -4a | 4 | 1, 2, 3a |
| REQ-NPCTALK-5 | 1 | — (ISA shape test) |
| REQ-NPCTALK-6 | 1, 3, 8, 9 | 10 |
| REQ-NPCTALK-7 | 4 | 3b |
| REQ-NPCTALK-8, -9, -10 | 3, 4 | — (parser tests), 22 case 1 |
| REQ-NPCTALK-11 | 9 | 5 |
| REQ-NPCTALK-12 | 9 | 9 |
| REQ-NPCTALK-13 | 9 | 10 |
| REQ-NPCTALK-14, -15, -15a | 9, 10 | 6, 7, 8 |
| REQ-NPCTALK-16 | 1 | 16 |
| REQ-NPCTALK-17 | 4, 8 | 4, 15 |
| REQ-NPCTALK-18, -18a | 6 | 17, 18, 19b |
| REQ-NPCTALK-19 | 6 | 19 |
| REQ-NPCTALK-20, -21, -22 | 5 | 11, 12 |
| REQ-NPCTALK-23 | 5 | 14 |
| REQ-NPCTALK-24 | 5, 11 | 13 |
| REQ-NPCTALK-25, -26 | 4 | 27, 28 |
| REQ-NPCTALK-27 | 4, 11 | 15 |
| REQ-NPCTALK-28 | 4 | 30 |
| REQ-NPCTALK-29 | 8 | 19a, and the `said`-before-`spoke` order |
| REQ-NPCTALK-29a | 2, 8 | 19a |
| REQ-NPCTALK-30 | 8 | 24 |
| REQ-NPCTALK-31 | 4, 7, 8 | 22, 23 |
| REQ-NPCTALK-32 | 7 | 20, 21 |
| REQ-NPCTALK-33 | 8, 11 | 24, 25 |
| REQ-NPCTALK-34 | 8 | 26 |
| REQ-NPCTALK-35 | 8 | 18 |
| REQ-NPCTALK-36 | — | stated, not built |
| REQ-NPCTALK-37 | 11 | — (carried by `testNpcStoreInvariants`) |
| REQ-NPCTALK-38 | — | recorded revisit condition |
| whole-spec | 11, 12 | 29, 31, 32, 33, 34 |

## Explicitly not built here

- Movement, goals, arrival, and major-character placement.
- The bard acting on what it reads in a conversation. REQ-NPCTALK-36 says a
  waking bard sees conversations for free; nothing acts on that.
- Talking to more than one character, or characters talking to each other.
- Streaming the reply. It is the strongest latency lever this feature has and it
  stays on the deferred list from the latency work — Step 12 item 6 collects the
  evidence for when to take it.
- Any path by which a character can give the player something. REQ-NPCTALK-38 is
  the revisit condition: the day that exists, social engineering becomes live and
  the decision moves out of the prompt and into engine code.

## Notes for whoever implements this

The load-bearing property of this brick is that **a conversation cannot write to
the world**. Two event rows, at most one profile, at most one memory summary —
and no code path to anything else. If a step here starts to want a component
write, the step is wrong, not the constraint.

The two places an error is silent rather than loud:

1. **The summary stamp** (Step 2). Off by one and every conversation loses its
   last exchange, forever, with nothing failing. The test is written to fail
   under the old behaviour on purpose.
2. **The resolver's ordering rule** (Step 9). Misclassify an action as speech
   and the character appears to ignore the player. The named eight-phrasing
   regression set is the contract; everything outside it is tuned against real
   play, and a green suite does not mean the boundary is settled.
