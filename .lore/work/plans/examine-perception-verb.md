---
title: "Implementation plan: examine — the perception verb"
date: 2026-08-06
status: executed
tags: [plan, examine, isa, perception, canon-description, scope, render, narration, resolver]
modules: [parser, action, systems, render, prose, nlresolve, mutations]
related: [.lore/work/specs/examine-perception-verb.md, .lore/work/design/examine-perception-verb.md, .lore/work/brainstorm/npcs.md, .lore/work/plans/ai-resolver.md]
---

# Implementation plan: examine — the perception verb

Adds the twelfth ISA verb, so every entity in the world can be looked at.
Source of truth: **[.lore/work/specs/examine-perception-verb.md]** (29
requirements, prefix `EXAMINE`, plus 27 numbered validation items).

The spec's controlling finding — *every entity that can appear already has a
`description` row* — is what makes this small. There is **no generation, no new
AI role, no new translation unit, no `SCHEMA_VERSION` bump, and no write beyond
one event row.** Everything below is a `SELECT`, a switch arm, and a prompt
sentence.

## Guiding constraints

- **Baseline before change.** Validation item 17 asks for byte-identical output
  on every *other* verb. A golden string captured after the change proves
  nothing, so Step 1 captures and pins it on unchanged code — it is the only
  step that must run first.
- **Deterministic throughout.** No step needs a live model. Every AI-path step
  is driven by canned responses through the existing injected-transport seams
  ([[verification-must-be-bounded]]). There is no live-smoke step in this
  feature and none should be added.
- **Reuse, don't rebuild.** `lookupNoun` (`src/lookup.hpp:16`), the
  `You don't see that here.` string (`src/systems.cpp:174`), `canonProseOf`'s
  SELECT shape (`src/prose.cpp:56`), and the existing clause/diagnostic
  machinery all already exist. Nothing here needs a new helper file.

## Seams this touches (verified in tree)

| Seam | File:line | What examine does with it |
|------|-----------|---------------------------|
| `Verb` enum | `src/action.hpp:11` | One member, `Examine`; the set becomes twelve — Step 2 |
| take/drop/read parser arm | `src/parser.cpp:73` | Copy its shape for `examine` / `x` — Step 2 |
| `resolveTake` scope shape | `src/systems.cpp:166` | `resolveExamine` mirrors it, minus the `portable` check — Step 2 |
| `resolveImpl` switch | `src/systems.cpp:193` | New `case Verb::Examine:` (exhaustive switch, so it lands in the same compile) — Step 2 |
| no-write verb contract | `src/mutations.hpp:10` | Comment gains `'examined'` — Step 2 |
| bard wake predicate | `src/bard.cpp:1043` | **Untouched.** `examined` is deliberately absent — Step 3 asserts it |
| `looked` render branch | `src/render.cpp:123` | New sibling `examined` branch — Step 4 |
| `TurnFacts` | `src/prose.hpp:30-35` | One new field beside `canonText` / `failedDetails` — Step 5 |
| `buildFacts` event scan | `src/prose.cpp:295-333` | Sets the new field; attaches `description` to the `examined` event object — Step 5 |
| narrator prompt | `src/prose.cpp:169` | One rule, beside the canon-verbatim rule — Step 5 |
| `validateAiResponse` clauses | `src/prose.cpp:255-271` | New clause f after e — Step 6 |
| `verbFromWord` | `src/nlresolve.cpp:96` | Twelfth arm — Step 7 |
| tool verb enum | `src/nlresolve.cpp:195` | Twelfth value, element-wise identical — Step 7 |
| resolver scope payload | `src/nlresolve.cpp:163-168` | Sixth key, `things` — Step 7 |
| `kResolveSystemPrompt` | `src/nlresolve.cpp:131` | Verb count, verb line, subject-source rule — Step 7 |
| `CMakeLists.txt:17` | — | **No change.** No new TU exists in this feature |

## Decisions taken (two asked, one stated)

1. **`things` includes the player.** The player entity has a `name` row
   (`'player'`) and a `location` row whose container is the room, in both
   `seed/base.sql:40` and `tests/fixture.sql:30`. REQ-EXAMINE-19's literal
   reading therefore puts `"player"` in `things` on every request, and the
   resolver can lower "look at myself" to `examine player`. That is consistent
   with REQ-EXAMINE-9 and is the chosen behavior — no filter is added.
2. **Item 17 gets a real golden-session test** (Step 1), not a lean on the
   existing per-verb assertions.
3. **The examined prose rides inside the event object, not at the top level.**
   REQ-EXAMINE-24/-25a need the description in the narrator's facts payload, but
   the spec does not say where. `tests/tests.cpp:1941` pins the payload's
   top-level key set to exactly the four REQ-PROSE-7 keys, so the prose is
   attached to the `examined` entry of the `events` array as a `description`
   key. REQ-PROSE-7 and its test stay intact.

## A consequence worth naming

`lookupNoun` is an exact first-match against the whole `name` value, so
`examine goblin` returns `nullopt` — the seeded enemy's name row is copied from
`bestiary.name`, which is `goblin grunt` (`seed/base.sql:77`, written by the
`SELECT` at `:105`). `examine goblin grunt` works, and so does the AI path,
which copies the noun verbatim out of `things`. This is existing `take`
behavior, not a regression, and the spec does not ask to change it.

## The fixture the parser tests need

Validation items 1–3 name **`candle`** in **the dormitory cell** — that is
`seed/base.sql`'s world (candle = entity 4, `base.sql:48-53`), **not**
`tests/fixture.sql`, whose entity 4 is `lantern` in a stone hall, and not
`tests/combat_fixture.sql`, whose entity 4 is `wand` in a room named `cell`.
`testParser` currently opens `tests/fixture.sql` (`tests.cpp:400`).

**Do not add a candle to `tests/fixture.sql`.** Its exact entity and row counts
are pinned by other tests — `combat_fixture.sql`'s own header says so, and that
is why it exists as a separate file. Instead, the new parser cases open a
**second world from `seed/base.sql`** inside `testParser`, the way
`testShippedSeedShape` (`tests.cpp:232`) and four other tests already do. The
existing `fixture.sql` block stays untouched.

---

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.5; padding: 8px 0;">
<b>1</b> golden baseline <span style="color:#b00;">◀ must be first</span><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>2</b> ISA verb + parser + resolveExamine ─┬─▶ <b>3</b> turn/bard/coverage tests<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>4</b> template render branch<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>5</b> facts field + payload + prompt ─▶ <b>6</b> clause f<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>7</b> resolver: enum, things, prompt<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>8</b> whole-feature sweep<br>
</div>

Steps 3–7 depend only on Step 2 and are independent of each other; Step 6 is the
one exception, needing Step 5's `TurnFacts` field. Risk legend:
<span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>
deterministic, mechanically verified — every step in this plan is LOW; there is
no live-LLM verification anywhere in this feature.

---

### Step 1 — Golden-session baseline for every pre-existing verb
**Requirements:** REQ-EXAMINE-23 (the "every other verb byte-identical" half). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

> ⚠️ **Runs on unchanged code, before Step 2.** A golden string captured after
> the change proves nothing.

Add `testExamineGoldenSession` to `tests/tests.cpp`. It opens
`tests/combat_fixture.sql` (the widest fixture: rooms, items, a grimoire, four
enemy archetypes, spells), runs one fixed script through the template path with
AI disabled, concatenates every `runTurn(...).output`, and compares the whole
thing to a string literal.

The script must touch every pre-existing verb at least once: `look`,
`inventory`, `wait`, `take`, `drop`, `read`, `go`, `attack`, `cast`, plus an
unparseable line for the tier-a `renderError` path. (`quit` and `spells` return
before the tick; include `spells` for its no-tick output, and leave `quit` out
since it produces none.)

Capture the literal by running the test once with the comparison printing the
actual output, then paste it in. Do not hand-write the expected text.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>cmake --build build && ./build/tests</code> is
green with the new test registered in <code>main()</code>, <b>on unchanged
engine code</b>. Commit this step by itself, so the baseline is in git history
before a single line of examine exists.
</blockquote>

### Step 2 — The ISA verb, the parser, and `resolveExamine`
**Requirements:** REQ-EXAMINE-1, -2, -3, -4, -5, -6, -7, -8, -9, -10, -11, -12, -13, -15, -17, -28. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Four files, one compile. The `Verb` switch in `resolveImpl` is exhaustive with
no `default`, so the enum member and its arm cannot be split across steps.

**`src/action.hpp:11`** — add `Examine` to the enum. Extend the `subject`
comment to name it (`entity id for Take/Drop/Examine; …`).

**`src/parser.cpp`** — a new arm beside the take/drop/read arm at line 73:

```cpp
if (verbWord == "examine" || verbWord == "x") {
    if (arg.empty()) return std::nullopt;        // bare verb, REQ-PROTO-6a
    const int64_t entity = lookupNoun(db, arg);
    if (entity == 0) return std::nullopt;        // not a noun anywhere in world
    Action a{Verb::Examine};
    a.subject = entity;
    return a;
}
```

The `look` arm at line 48 is **not touched** (REQ-EXAMINE-4).

**`src/systems.cpp`** — `resolveExamine` in the anonymous namespace, next to
`resolveTake`, and a `case Verb::Examine:` in `resolveImpl`. It reuses
`roomOf` / `containerOf` (already there at lines 18 and 29) and deliberately
does **not** call `isPortable` — that is the whole difference from `take`:

```cpp
void resolveExamine(Db& db, const Action& action, int64_t player) {
    const int64_t room = roomOf(db, player);
    const std::optional<int64_t> where = containerOf(db, action.subject);
    if (!where || (*where != room && *where != player)) {
        appendEvent(db, player, "failed", 0, 0, "You don't see that here.");
    } else {
        appendEvent(db, player, "examined", action.subject, 0, nullptr);
    }
}
```

No new refusal string, no component write, no `description` read here — the
text is the renderer's job (REQ-EXAMINE-10/-11 land in Step 4).

**`src/mutations.hpp:10`** — the no-write-verb contract comment gains
`'examined'`: *"…legal ONLY for the no-write verbs: 'looked', 'waited',
'failed', 'examined'."*

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> build clean, Step 1's golden test still green, and:
<br>• <code>testParser</code> (<code>tests.cpp:398</code>) gains items 1–4, in a
<b>second world opened from <code>seed/base.sql</code></b> (see "The fixture the
parser tests need" above) — <code>examine candle</code> and <code>x candle</code>
both yield <code>Action{Verb::Examine, subject = 4}</code>; bare
<code>examine</code> and <code>x</code> yield <code>nullopt</code>;
<code>examine gryphon</code> yields <code>nullopt</code>; and the
<b>regression</b> that <code>look</code>,
<code>look around</code>, and <code>look at the candle</code> all still yield a
bare <code>Action{Verb::Look}</code> with <code>subject == 0</code>.
<br>• <code>testSystems</code> (<code>tests.cpp:524</code>) gains items 5, 6, 7,
8, 8a, 11, 12, 13 — examine in room, examine carried, examine elsewhere
(exactly <code>You don't see that here.</code> <b>and</b> <code>meta.turn</code>
advanced by one), exactly one <code>examined</code> row with
<code>object = 0</code> and NULL detail, no component-table row count changes
across the turn.
<br>• The six-verb assertion at <code>tests.cpp:640</code> gains
<code>'examined'</code> if an examine tick is added to that world.
</blockquote>

### Step 3 — Turn cost, bard silence, and the description coverage guard
**Requirements:** REQ-EXAMINE-14, -16, -27 (guard). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Test-only; no source changes. Three cross-system assertions that do not belong
in `testSystems`. Item 9 (byte-equality) is **not** here — it needs Step 4's
render branch, so it lives there.

- **Item 14 (turn cost under fire).** In `tests/combat_fixture.sql`, with a
  hostile in the room, one examine turn produces the enemy's `chip` event in the
  same transaction. `resolveCombat` already runs for every ticked action
  (`loop.cpp:126`), so this is an assertion, not a change.
- **Item 15 (the bard stays asleep).** Extend the quiet-verb loop at
  `tests.cpp:12033` — currently `{"moved", "took", "looked", "waited",
  "failed"}` — with `"examined"`. `src/bard.cpp:1043`'s predicate is unchanged,
  which is the point.
- **Item 10 (coverage guard).** Build a world containing one of each of the five
  writers' outputs and assert **every named entity has a `description` row**:
  seeded rooms/items from the fixture, plus `dropGrimoire`, `placeEnemy`,
  `writeGeneratedRoom`, and `writeCatalogEntry` + `placeCatalogEntry` called
  directly (all declared in `src/mutations.hpp`, all callable from tests). The
  player is the one documented exemption and must be excluded by name, with a
  comment pointing at `base.sql`'s deliberate omission.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> all four assertions pass; the coverage guard fails
loudly if a sixth writer ever mints an entity without prose. No source file
changed in this step — confirm with <code>git diff --stat src/</code> being
empty for it.
</blockquote>

### Step 4 — The template render branch
**Requirements:** REQ-EXAMINE-10, -11, -17 (proof), -23. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`src/render.cpp` gains **exactly one** branch, a sibling of the `looked` branch
at line 123, plus one read-only helper beside `nameOf` (line 17):

```cpp
} else if (verb == "examined") {
    Stmt s = db.prepare("SELECT prose FROM description WHERE entity = ?");
    s.bind(1, subject);
    out += s.step() ? s.colText(0) + "\n"
                    : "You see nothing special about the " +
                      nameOf(db, subject) + ".\n";
}
```

Prose verbatim, no wrapping or trimming (REQ-EXAMINE-10); the fallback line
exactly as REQ-EXAMINE-11 spells it. Every other branch is untouched, and
`render.cpp`'s read-only contract holds — this is a `SELECT`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> item 16 — <code>examined</code> output byte-exact for
both cases, asserted in <code>testRender</code>
(<code>tests.cpp:1668</code>). Item 9 — examine the seeded goblin and assert the
rendered output is <b>byte-equal</b> to its <code>description</code> row and
contains nothing else; equality, not digit-hunting, is what actually proves
REQ-EXAMINE-17, since nothing derived from <code>health</code>,
<code>hostile</code>, <code>resistance</code>, or any status table can be
present if the output is the description row exactly. Item 17 — <b>Step 1's
golden test still passes unmodified</b>, which is the byte-identity proof for
the other eleven verbs.
</blockquote>

### Step 5 — Narrator facts: the new anchor, the payload key, the prompt rule
**Requirements:** REQ-EXAMINE-24, -25a (facts half), -22 (no ids). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

**`src/prose.hpp:30-35`** — one field on `TurnFacts`, documented as
empty-means-inactive:

```cpp
// Canon prose of an entity examined this turn (REQ-EXAMINE-25), verbatim from
// the description table. EMPTY means clause f DOES NOT APPLY — either no
// 'examined' event this turn, or the entity has no description row
// (REQ-EXAMINE-25a). It never means "the empty string was not found".
std::string examinedText;
```

**`src/prose.cpp`** — in the event scan (lines 295–333), beside the existing
`canonRequired` / `failedDetails` assignments:

- on an `examined` event, look the subject's prose up with the existing
  `canonProseOf` (line 56) and set `facts.examinedText` to it (or leave it empty
  when there is no row);
- attach it to that event's JSON object as `e["description"]` when present. When
  absent, the event object carries the subject's **name only** — which the
  existing `e["subject"]` assignment already provides (REQ-EXAMINE-25a).

No top-level payload key is added: the four REQ-PROSE-7 keys and their
`p.size() == 4` assertion at `tests.cpp:1941` are untouched. `deterministicAppends`
(line 125) is untouched too — examine appends nothing mechanical.

**`src/prose.cpp:169`** — one rule beside the canon-verbatim rule:

> `- If an event carries a description, include its text verbatim, word for word and unmodified. Write your connective prose around it, never inside it.`

Without this the model has no instruction to reproduce the prose, clause f would
fail on every examine turn, and the AI path would silently degrade to the
template.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new assertions in <code>testProseFacts</code>
(<code>tests.cpp:1921</code>): an examine turn sets <code>examinedText</code> to
the description row verbatim and the <code>examined</code> event object carries
a matching <code>description</code>; an examine of an entity with no description
row leaves <code>examinedText</code> empty and the event object carrying
<code>subject</code> only. Top-level key count still 4. <code>checkPayloadHygiene</code>
still passes (no ids, REQ-EXAMINE-22). Prompt substring test extended for the
new rule.
</blockquote>

### Step 6 — Clause f on the validation gate
**Requirements:** REQ-EXAMINE-25, -25a (gate half), -26. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`src/prose.cpp` — one clause **after** clause e (line 270), and the contract
comment in `prose.hpp:63-74` extended to list it:

```cpp
// Clause f: examined canon prose verbatim (REQ-EXAMINE-25). An EMPTY
// examinedText means the clause does not apply (REQ-EXAMINE-25a) — never
// that the empty string was missing.
if (!facts.examinedText.empty() &&
    text.find(facts.examinedText) == std::string::npos) {
    return failClause('f', "examined canon description not present verbatim");
}
```

Clauses a–e keep their behavior, their order, and their diagnostic wording
exactly. The template fallback on failure needs **no new code**: `aiRender`
already returns `nullopt` on a rejected response and `loop.cpp:141-146` already
falls through to `render()`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> in <code>testProseValidation</code>
(<code>tests.cpp:3759</code>), over canned responses only: item 18 — a response
containing the canon prose verbatim is accepted, a paraphrase is rejected, and
the diagnostic names clause <b>f</b>. Item 18a — with an entity that has no
description row, a response containing <b>none</b> of the template wording is
accepted and clause f never fires. Item 19 — <b>clauses a–e reject and accept
exactly as before, with the existing canned responses unmodified</b>. Item 20 —
a clause-f rejection drives <code>runTurn</code> to the template output and the
turn still prints.
</blockquote>

### Step 7 — The resolver: twelfth verb, `things`, and the prompt
**Requirements:** REQ-EXAMINE-18, -19, -20, -21, -22. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

The only part with real surface area, and the design names it as the risk: a
twelfth verb and a wider payload can quietly degrade the other eleven.

**`src/nlresolve.cpp:96`** — `verbFromWord` gains
`if (word == "examine") return Verb::Examine;`.

**`src/nlresolve.cpp:195`** — `"examine"` joins the tool's verb enum array. The
two lists stay element-wise identical.

**`src/nlresolve.cpp:163-168`** — a sixth payload key beside `items`:

```cpp
payload["things"] = json(namedEntitiesIn(db, room));
```

with a new read-only helper next to `portableNamesIn` (line 69):

```cpp
// Noun words of ALL named entities in a room, portable or not, in entity order
// (REQ-EXAMINE-19). Wider than portableNamesIn deliberately: enemies,
// characters, and fixed scenery are examinable and none of them are portable.
// The player is included — their container is the room and no case excludes
// them (REQ-EXAMINE-9).
std::vector<std::string> namedEntitiesIn(Db& db, int64_t room) {
    std::vector<std::string> names;
    Stmt s = db.prepare(
        "SELECT n.value FROM name n "
        "JOIN location l ON l.entity = n.entity "
        "WHERE l.container = ? ORDER BY n.entity");
    s.bind(1, room);
    while (s.step()) names.push_back(s.colText(0));
    return names;
}
```

`items` keeps its current meaning and contents, so nothing that reads it changes
behavior (REQ-EXAMINE-19).

**`src/nlresolve.cpp:131`** — three edits to `kResolveSystemPrompt`:
its scope-facts sentence gains `"things"`; `exactly eleven verbs` becomes
`exactly twelve verbs` with a new `- examine:` line in the same style as the
others; and the subject rule becomes *"one of the noun words supplied in
`items`, `inventory`, or `things`"*. The "introduce no noun absent from the
scope facts" rule is unchanged in force.

**No guard is added** for `take` on a non-portable thing (REQ-EXAMINE-21) —
`resolveTake` already answers `You can't take that.`

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b>
<br>• Item 21 — the element-wise equality test inside
<code>testSpellsVerb</code> (<code>tests.cpp:9218</code>) extended to twelve:
the word list gains <code>"examine"</code>, and its <code>arms == words.size()</code>
count follows.
<br>• Item 22 — in <code>testNlResolveContext</code>
(<code>tests.cpp:2079</code>), a captured body for a room containing the goblin
has <code>things</code> including <code>"goblin grunt"</code>, and
<code>items</code> is <b>unchanged from what it asserts today</b>. The exact
top-level key assertions at <code>tests.cpp:2093</code> and
<code>:2119</code> move from <code>p.size() == 5</code> to <code>== 6</code>.
<br>• Item 23 — <code>checkPayloadHygiene</code> over the new body: no integer
id, no <code>tier</code>, no <code>seeded</code> flag, no internal tag anywhere.
<br>• Item 24 — <b>regression:</b> the existing resolver phrasing tests for all
eleven prior verbs pass unmodified against a room that now also supplies
<code>things</code>.
<br>• <code>testNlResolvePrompt</code> (<code>tests.cpp:2143</code>) and the
prompt checks at <code>tests.cpp:9246</code> updated:
<code>"exactly twelve verbs"</code> present, <code>"exactly eleven verbs"</code>
absent, <code>"\n- examine:"</code> present.
</blockquote>

### Step 8 — Whole-feature validation sweep
**Requirements:** all (validation sweep), REQ-EXAMINE-28, -29. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Walk the spec's 27 numbered validation items end to end and confirm each is
asserted somewhere, then the three whole-feature gates:

- **Item 25 (no writes).** The spec's grep is over "any new translation unit
  this feature adds" — this feature adds **none**, so run it over the diff
  instead: `git diff main -- src/ | grep -En "^\+.*(INSERT|UPDATE|DELETE)"` is
  empty. Every write in the feature goes through `appendEvent`.
- **Item 26 (offline).** `./build/tests` green with `ANTHROPIC_API_KEY` unset
  and no network. The suite already unsets it hermetically at
  `tests.cpp:13195`.
- **Item 27 (no schema movement).** `SCHEMA_VERSION` in `src/world.cpp`
  unchanged, and a `world.db` created before this change opens and plays. The
  repo has one at the project root to test against — copy it, don't play the
  original.
- **REQ-EXAMINE-29 (band unchanged).** `git diff --stat src/band.cpp src/band.hpp`
  is empty.
- **REQ-EXAMINE-12 (`AiRole` still three).** `git diff --stat src/aihttp.hpp`
  shows no `AiRole` change.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> every one of the 27 validation items maps to a named
test or a named command, and each of the 29 requirements appears in the coverage
map below. This step is the plan's contract with the spec.
</blockquote>

---

## Requirement coverage map

| Requirement | Step(s) |
|-------------|---------|
| REQ-EXAMINE-1 (`Verb::Examine`) | 2 |
| REQ-EXAMINE-2 (reaches `resolve` inside the tick) | 2 |
| REQ-EXAMINE-3 (`examine` / `x`, nothing else) | 2 |
| REQ-EXAMINE-4 (`look` unchanged) | 2 (regression assertion) |
| REQ-EXAMINE-5 (bare verb → `nullopt`) | 2 |
| REQ-EXAMINE-6 (unknown noun → `nullopt`, shared `lookupNoun`) | 2 |
| REQ-EXAMINE-7 (scope: room or carried, no `portable`) | 2 |
| REQ-EXAMINE-8 (`You don't see that here.`) | 2 |
| REQ-EXAMINE-9 (player in scope) | 2 (item 8a), 7 (`things` includes them) |
| REQ-EXAMINE-10 (description verbatim) | 4 |
| REQ-EXAMINE-11 (fallback line) | 4 |
| REQ-EXAMINE-12 (no model call, `AiRole` stays 3) | 2, 8 |
| REQ-EXAMINE-13 (one `examined` event; contract comment) | 2 |
| REQ-EXAMINE-14 (not a bard wake trigger) | 3 |
| REQ-EXAMINE-15 (`looked` not overloaded) | 2 |
| REQ-EXAMINE-16 (costs a turn; enemies act) | 2 (turn), 3 (enemy) |
| REQ-EXAMINE-17 (nothing mechanical) | 4 (byte-equality) |
| REQ-EXAMINE-18 (enum + `verbFromWord`) | 7 |
| REQ-EXAMINE-19 (`things`, `items` unchanged) | 7 |
| REQ-EXAMINE-20 (prompt: count + subject sources) | 7 |
| REQ-EXAMINE-21 (no guard on `take <non-portable>`) | 7 |
| REQ-EXAMINE-22 (no ids on the wire) | 5, 7 |
| REQ-EXAMINE-23 (one render branch, others byte-identical) | 1 (baseline), 4 |
| REQ-EXAMINE-24 (AI path: prose verbatim) | 5, 6 |
| REQ-EXAMINE-25 (clause f, a–e untouched) | 6 |
| REQ-EXAMINE-25a (empty = inactive) | 5, 6 |
| REQ-EXAMINE-26 (clause-f failure → template) | 6 |
| REQ-EXAMINE-27 (no generation, ever) | 3 (coverage guard), 8 |
| REQ-EXAMINE-28 (no write beyond the event row) | 2, 8 |
| REQ-EXAMINE-29 (status band unchanged) | 8 |

## Validation-item coverage map

| Items | Step |
|-------|------|
| 1–4 (parser) | 2 |
| 5, 6, 7, 8, 8a (scope and text) | 2 |
| 10 (coverage guard) | 3 |
| 11, 12, 13 (event and turn) | 2 |
| 14, 15 (enemy turn, bard quiet) | 3 |
| 9, 16 (byte-equality, template byte-exact) | 4 |
| 17 (every other verb byte-identical) | 1 (captured), 4 (proved) |
| 18, 18a, 19, 20 (AI path, clauses) | 6 |
| 21, 22, 23, 24 (resolver) | 7 |
| 25, 26, 27 (whole-feature gates) | 8 |

All 29 requirements and all 27 validation items are covered. No step adds a
translation unit, a build dependency, an AI role, a write path, or a schema
change.
