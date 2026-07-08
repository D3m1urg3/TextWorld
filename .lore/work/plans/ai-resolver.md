---
title: "Implementation plan: ai-resolver"
date: 2026-07-08
status: executed
tags: [plan, ai-integration, nl-resolver, claude-api, tool-use, action-isa, parser, fallback]
modules: [parser, action, loop, nlresolve]
related: [.lore/work/specs/ai-resolver.md, .lore/work/brainstorm/ai-resolver.md, .lore/work/specs/ai-prose-renderer.md, .lore/work/specs/engine-foundation-prototype.md]
---

# Implementation plan: ai-resolver

Lowers a raw input line into the engine's closed `Action` ISA via Claude
tool-use, with the fixed-verb parser as the permanent deterministic fallback.
Source of truth: **[.lore/work/specs/ai-resolver.md]** (16 requirements, prefix
`RESOLVE`). This plan sequences those requirements into atomic, mostly-deterministic
steps that **mirror the shipped prose renderer** (`src/prose.cpp`, `src/prose.hpp`,
`tests/tests.cpp`) rather than reinventing its seams.

## Guiding constraints (from memory + spec)

- **Deterministic skeleton first, LLM last.** Steps 1–8 are fully unit-testable
  with no network; the single live-LLM step (9) is isolated and gated. This is
  the [[token-risk-estimation]] / [[verification-must-be-bounded]] discipline:
  the token trap is open-ended live verification, not diff size.
- **Reuse proven prose seams, don't rebuild them:**
  - `HttpResponse` / `HttpTransport` types → included from `prose.hpp` (not redefined).
  - `aiNarrationEnabled()` → **reused verbatim** as the resolver's enable switch
    (REQ-RESOLVE-2: *one* flag governs both AI features).
  - Live-smoke gate `TEXTWORLD_AI_LIVE_TEST=1`, injectable-fake test discipline,
    db-byte-identity + no-egress contracts → copied from the prose tests.
- **One seam / one file / one testable behavior per step.** Target ≈ prose's
  8-commit granularity.

## Seams this touches (verified in tree)

| Seam | File:line | What the resolver does with it |
|------|-----------|-------------------------------|
| `lookupNoun` (private, anon ns) | `src/parser.cpp:34` | **Hoist to shared header** so the gate (REQ-RESOLVE-13c) reuses it — Step 1 |
| `Action` / `Verb` | `src/action.hpp:11` | Resolver emits this; TU must stay free of parser types |
| `HttpResponse` / `HttpTransport` | `src/prose.hpp:48-53` | Included and reused as-is |
| `aiNarrationEnabled()` | `src/prose.cpp:410` | Reused as the resolver's enable check |
| `runTurn` dispatch | `src/loop.cpp:32-38` | `aiResolve` inserted *before* `parse` — Step 7 |
| mutating `resolve()` | `src/systems.cpp:83` | Name collision avoided → new TU is `nlresolve` |
| twcore sources list | `CMakeLists.txt:17` | Add `src/nlresolve.cpp` — Step 2 |
| test runner | `tests/tests.cpp:1550` `main()` | Register each `testNlResolve*` |

## Two micro-decisions (flagged, not blocking)

1. **`lookupNoun` hoist form.** Recommend an `inline int64_t lookupNoun(Db&, const std::string&)`
   in a new header **`src/lookup.hpp`** (header-only, no CMake change, no ODR concern).
   Alternative (heavier, matches the repo's .hpp+.cpp pairing) is `lookup.hpp` + `lookup.cpp`
   added to twcore. Header-only is the smaller, recommended path.
2. **Production HTTP transport.** `prose.cpp`'s `curlTransport` is anon-namespace-private.
   Mirror prose's own "deliberately reimplement" stance: give `nlresolve.cpp` its own
   small `curlTransport` (same URL / headers / 8 s timeout). Only the `HttpTransport`
   *type* is shared, per REQ-RESOLVE-11. (Optional future cleanup: hoist a shared
   `http.hpp` transport — out of scope here.)

---

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.5; padding: 8px 0;">
<b>1</b> hoist lookupNoun ─┐<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>5</b> validation+mapping gate ──┐<br>
<b>2</b> TU + scope ctx ──┼─▶ <b>4</b> request body ─────────────┼─▶ <b>6</b> aiResolve orchestration<br>
<b>3</b> ISA prompt ──────┘&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;(3 feeds 4)&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>7</b> loop dispatch + parser promotion<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>8</b> tier-b passthrough test<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>9</b> live smoke <span style="color:#b00">[HIGH token-risk — isolated]</span><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>10</b> final validation vs spec checklist<br>
</div>

Risk legend: <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified · <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH</span> live-LLM verification.

---

### Step 1 — Hoist `lookupNoun` to a shared header
**Requirements:** REQ-RESOLVE-14. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Standalone refactor; nothing AI here. Create **`src/lookup.hpp`** containing the
exact body currently at `src/parser.cpp:34-39` as an `inline` function (world-wide
first-match name→entity lookup, returns 0 on no match). Remove it from `parser.cpp`'s
anonymous namespace; `#include "lookup.hpp"` there instead. Behavior of `parse()`
(REQ-PROTO-6a bare-verb `nullopt`, first-match, case handling) is **unchanged**.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>cmake --build build</code> succeeds; existing
<code>testParser</code> (tests.cpp:191, incl. <code>take lantern</code>→subject 4,
<code>take zeppelin</code>→nullopt) passes unchanged. No new test needed — the hoist
is behavior-preserving.
</blockquote>

### Step 2 — New TU + scope-context builder
**Requirements:** REQ-RESOLVE-14 (TU), REQ-RESOLVE-7, REQ-RESOLVE-6. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Create **`src/nlresolve.hpp` / `src/nlresolve.cpp`**; add `src/nlresolve.cpp` to
the twcore sources at `CMakeLists.txt:17` (`add_library(twcore STATIC …)`). The header `#include "prose.hpp"` (for
`HttpResponse`/`HttpTransport`) and `#include "action.hpp"` — and **must not** include
`parser.cpp` types. Header comment states the read-only + network-egress contract,
mirroring `prose.hpp:1-9`.

Implement a pure `buildResolveContext(Db&, const std::string& line)` →
`ResolveContext { std::string payload; }` (JSON string for the user message)
containing **exactly** the REQ-RESOLVE-7 fields and no ids (REQ-RESOLVE-6):
raw input line · current room name · exit direction words · visible item names in
room · actor inventory item names. Reuse the same SELECT shapes prose already uses
(`portableNamesIn`, `exits`, `nameOf`, `roomOf` patterns from `prose.cpp:29-67`).
The builder takes no actor parameter, so it resolves the player entity itself via
`SELECT entity FROM player LIMIT 1` (mirroring `loop.cpp:18`'s `playerId()` and
`buildFacts`'s player fallback at `prose.cpp:352-356`) and slices room/items/inventory
relative to it.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testNlResolveContext</code> against the seeded
world asserts the payload contains exactly the 5 fields (line, room name, exits,
room items, inventory) and <b>no entity/row ids anywhere</b> (parse back as JSON,
assert key set). Callable with no network.
</blockquote>

### Step 3 — The ISA system prompt (the "actual work")
**Requirements:** REQ-RESOLVE-12. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Add a `kSystemPrompt` constant to `nlresolve.cpp` (git-versioned, like
`prose.cpp:149`). It **is** the ISA spec: defines all seven verbs
(Look/Go/Take/Drop/Inventory/Wait/Quit) crisply and **non-overlappingly**, and states
each rule — translate to exactly one action; emit via `emit_action`; `subject` must
be one of the supplied in-scope noun names verbatim; introduce no noun absent from
the context; `direction` is a movement/compass word for `go`; at most one action
(multi-intent → no tool call); unknown/no-single-action → no tool call; **no
pronoun/anaphora resolution** (v2).

Prompt *quality* is verified live (Step 9) — here we only pin its structure
mechanically, keeping this step cheap per the bounded-verification rule.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> a substring spot-check test (mirroring the prose
prompt tests) asserts the constant mentions each of the seven verb names and the
"one action" / "in-scope noun" / "no new nouns" / "no pronoun" rules. Manual
prompt-content inspection = spec AI-Validation item 3.
</blockquote>

### Step 4 — Request body + `emit_action` tool schema
**Requirements:** REQ-RESOLVE-8, REQ-RESOLVE-9. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`buildResolveRequestBody(const std::string& contextPayload)` → JSON string, built
with nlohmann/json exactly like `prose.cpp:226`. Differences from prose per spec:
`max_tokens` **512**; a `tools` array carrying **one** `emit_action` tool whose
input schema has a **schema-enforced `verb` enum of exactly the seven ISA verbs**,
an optional `subject` string, an optional `direction` string; `tool_choice: auto`.
Same as prose: model `claude-opus-4-8` overridable via `TEXTWORLD_MODEL`,
`anthropic-version` header, **no** `thinking`, **no** `stream`, **no** cache keys.
`system` = the Step-3 `kSystemPrompt`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testNlResolveRequestBody</code> parses the output
back as JSON and asserts: <code>emit_action</code> tool present with a 7-value verb
enum; <code>tool_choice==auto</code>; <code>max_tokens==512</code>; model default +
<code>TEXTWORLD_MODEL</code> override; and — mirroring <code>testProseRequestBody</code>'s
<code>j.size()==4</code> stray-key guard (tests.cpp:999) — assert the <b>exact top-level
key set</b> so no <code>thinking</code>/<code>stream</code>/cache-control key can slip in.
Pure string→string (plus env read).
</blockquote>

### Step 5 — Validation & mapping gate
**Requirements:** REQ-RESOLVE-13 (a–e), REQ-RESOLVE-14 (reuses hoisted lookup). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

The resolver's analog of `validateAiResponse`. A pure function
`validateAndLower(const HttpResponse&, Db&)` → `std::optional<Action>` that **never
throws** and emits one stderr diagnostic naming the first failed clause:
- **a.** HTTP 200 and **exactly one** `tool_use` block for `emit_action` (0 or ≥2 → fail; ≥2 enforces one-opcode-per-line).
- **b.** `verb` is exactly one of the seven ISA verbs.
- **c.** `take`/`drop`: `subject` present and `lookupNoun` (Step 1) resolves it to a **non-zero** id — **world-wide recognition, NOT scope applicability** (applicability is the engine's tier-b job). Id assigned mechanically, never from the model.
- **d.** `go`: `direction` present and non-empty.
- **e.** `look`/`inventory`/`wait`/`quit`: no argument consulted.

`no tool call` (0 blocks) → `nullopt`, cleanly.

**Fixture shape (pin it — do NOT make a live call to discover it).** An Anthropic
tool-use response body is `{"stop_reason":"tool_use","content":[{"type":"tool_use",
"id":"toolu_…","name":"emit_action","input":{"verb":"take","subject":"lantern"}}]}`
(a `go` call carries `"direction"` instead of/alongside `subject`; bare verbs carry
neither). The gate navigates `content[]`, counting blocks with `type=="tool_use"` &&
`name=="emit_action"` (clause a needs exactly one), then reads `input.verb` /
`input.subject` / `input.direction`. Add a `cannedToolUse(verb, subject, direction)`
test helper that emits exactly this shape — the resolver analog of prose's
`cannedResponse` (`prose.cpp:214-224` / `tests.cpp:1055-1065`) — so fixtures are built
from documented structure, never a network probe.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testNlResolveGate</code> feeds
<code>cannedToolUse</code> fixtures covering <b>each clause a–e</b> plus no-tool-call
(0 blocks), two-tool-call (≥2 blocks), out-of-set verb, and unknown subject → all the
right <code>Action</code>/<code>nullopt</code>. Uses seeded db for the clause-c lookup.
No network. Function never throws (called outside any try/catch, like prose's gate).
</blockquote>

### Step 6 — `aiResolve` orchestration + production transport
**Requirements:** REQ-RESOLVE-1 (resolver half), -2, -3, -5, -10, -11. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Wire the pipeline, mirroring `aiRender`'s two overloads (`prose.cpp:420-455`):
- `std::optional<Action> aiResolve(Db&, const std::string& line, const HttpTransport&)`
  — test-visible: `buildResolveContext` → `buildResolveRequestBody` → **one** transport
  call → `validateAndLower`. Whole body in try/catch; any failure → one stderr line +
  `nullopt` (REQ-RESOLVE-3). Transport invoked **at most once**, no retries.
- `std::optional<Action> aiResolve(Db&, const std::string& line)` — production; binds a
  local `curlTransport` (same URL/headers/**8 s** timeout as prose; micro-decision #2).
- **Enable check:** reuse `aiNarrationEnabled()` from `prose.hpp` (REQ-RESOLVE-2, one
  switch) — no new predicate.

Optional commit split for a cleaner boundary (still one step here): **6a** the pure
orchestrator + validation wiring (unit-tested below), **6b** the ~40-line libcurl
`curlTransport` reimplementation (copy-adjacent to `prose.cpp:176-212`: header-list
lifetime, `curl_slist_free_all`, `CURLOPT_TIMEOUT` in seconds). 6b has no direct unit
test — it is exercised only live in Step 9 — so isolating it keeps that untested diff
independently reviewable.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testNlResolveAiResolve</code> with fake transports
(mirroring <code>testProseTransport</code>): canned <code>emit_action</code> → correct
<code>Action</code>; transportError / malformed body / <b>throwing</b> transport →
<code>nullopt</code>; call-count == 1; and <b>db byte-identity</b> across
<code>aiResolve</code> (REQ-RESOLVE-5). Plus <code>grep -En "INSERT|UPDATE|DELETE"
src/nlresolve.cpp</code> returns nothing.
</blockquote>

### Step 7 — Loop dispatch + parser promotion (with a testable fall-through seam)
**Requirements:** REQ-RESOLVE-1, -2, -4, **-15 (dispatch-fallback clause)**. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Introduce a small pure fall-through helper in `nlresolve` so the
`aiResolve → parse` chain is unit-testable **without a live call** (the gap the loop's
hardwired production transport would otherwise leave — mirroring how `loop.cpp` hardwires
`aiRender`):

- `std::optional<Action> resolveOrParse(Db&, const std::string& line, const HttpTransport&)`
  — calls `aiResolve(db, line, transport)`; on `nullopt` falls to `parse(db, line)`;
  returns the first that yields an `Action`, else `nullopt`.
- Production `resolveOrParse(Db&, const std::string& line)` binds the libcurl transport.

In `runTurn` (`loop.cpp:32`), before resolution: when `aiNarrationEnabled()`, use
`resolveOrParse(db, line)`; otherwise call `parse(db, line)` directly (disabled mode
never constructs a transport). When the chosen path is `nullopt`, the existing tier-a
`renderError("I don't understand that.")` fires (no tick). Order:
**aiResolve → parse → renderError.** `#include "nlresolve.hpp"`. Everything downstream
(Quit-before-tick, the tick transaction, `resolve`, narration dispatch) is untouched.

Also **rewrite `parser.cpp`'s header comment** (`parser.cpp:1-6`): it is no longer
"DISPOSABLE / deleted without ceremony" — it is the **permanent deterministic
fallback** for input, the input-side analog of `render.cpp`'s template fallback
(REQ-RESOLVE-4). Behavior unchanged.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testNlResolveDispatch</code> exercises the full
REQ-RESOLVE-15 fallback chain <b>deterministically</b> with a fake no-tool-call
transport: <code>resolveOrParse(db, "take lantern", fake)</code> → resolver declines →
<b>parser</b> yields <code>Take</code>; <code>resolveOrParse(db, "smell the flowers",
fake)</code> → both decline → <code>nullopt</code> (the value that drives
<code>renderError</code> in <code>runTurn</code>). Plus existing <code>testLoop</code>
(tests.cpp:551) passes unchanged (no key → <code>aiNarrationEnabled()</code> false →
parser path byte-identical to today). Confirm <code>parser.cpp</code> header no longer
says "DISPOSABLE". Manual fallback run = spec AI-Validation item 4.
</blockquote>

### Step 8 — Tier-b passthrough test (recognition vs resolution)
**Requirements:** REQ-PROTO-6b via the boundary declaration (spec AI-Validation 4b). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

A dedicated deterministic test proving the resolver never pre-empts the engine's
applicability authority. (Depends only on Step 6's `aiResolve` + the engine's `resolve`
— it drives them directly, not through `runTurn`, so it does **not** actually require
Step 7's loop wiring and could run in parallel with it.) With a fake transport that resolves `take <item>` for an item
that **exists world-wide but is not in the actor's room**, the gate (clause c, recognition
only) passes and `aiResolve` returns a valid `Take` Action; driving it through a tick
(`resolve`) then **ticks the world, emits a `failed` event, and neither retries nor
re-calls the resolver.**

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testNlResolveTierBPassthrough</code>: assert the
resolved <code>Action</code>, then that resolving it in a tick produces a
<code>failed</code> event row and a single transport call. Fully deterministic.
</blockquote>

### Step 9 — Live end-to-end smoke (isolated, gated)
**Requirements:** REQ-RESOLVE-16. **Size:** S (code) · **Token-risk:** <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH — live LLM</span>

> ⚠️ **This is the one high-token-risk step** ([[verification-must-be-bounded]]).
> It is deliberately last, isolated, and its assertions are **mechanical only** so it
> cannot become a tune-retry loop. Do **not** turn it into prompt-tuning iterations
> against live output; if the prompt needs work, that is a bounded, separate effort.

Add `testNlResolveLiveSmoke`, structured exactly like `testProseLiveSmoke`
(tests.cpp:1397): returns immediately unless `TEXTWORLD_AI_LIVE_TEST=1` (skipped in
default run, excluded from any future CI), reads env without mutating it. Drives real
phrasings through the **production** transport — "pick up the lantern", "grab lantern",
"head north" — and asserts **mechanical invariants only**: each lowers to the expected
verb/subject `Action` **or** falls back cleanly; a nonsense line ("smell the flowers")
makes no tool call → `nullopt` → parser fallback → `renderError`. **Never** assert
model-specific wording.

<blockquote style="border-left:4px solid #b00;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> default <code>./build/tests</code> run <b>skips</b> it
(no network). Under <code>TEXTWORLD_AI_LIVE_TEST=1</code> with a real key, the three
phrasings yield expected-verb Actions or clean fallback, and the nonsense line falls
through — asserted structurally, run rarely.
</blockquote>

### Step 10 — Final validation against the spec checklist
**Requirements:** all (validation sweep). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Walk the spec's **AI Validation** section (items 1–8) end to end and confirm each:
1. Build check (REQ-RESOLVE-14) — `cmake -B build && cmake --build build`, no new packages; `nlresolve.{cpp,hpp}` present, no parser types; hoisted `lookupNoun` shared header included by both `parser.cpp` and `nlresolve.cpp`.
2. Contract grep (REQ-RESOLVE-5) — `grep -En "INSERT|UPDATE|DELETE" src/nlresolve.cpp` empty.
3. Prompt inspection (REQ-RESOLVE-12) — seven verbs non-overlapping + every rule present.
4 / 4b. Fallback run + tier-b passthrough — no key: parser handles `look`/`take key`/`go north`/`inventory` + unknown → `renderError`; parser header no longer "DISPOSABLE".
5. Kill-switch (REQ-RESOLVE-2) — key set but `TEXTWORLD_AI=0` matches item 4.
6. Unit suite (REQ-RESOLVE-6/7/8/11/13/15) — `./build/tests` green incl. all new `testNlResolve*`.
7. Timeout/failure (REQ-RESOLVE-3/10) — fake timeout/malformed/throwing → `nullopt`, within bound, no crash, nothing AI-flavored leaks.
8. Live smoke (REQ-RESOLVE-16) — manual/optional per Step 9.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> every checklist item passes; each maps back to a named
requirement. This step is the plan's contract with the spec.
</blockquote>

---

## Requirement coverage map

| Requirement | Step(s) |
|-------------|---------|
| REQ-RESOLVE-1 (dispatch/fallback) | 6, 7 |
| REQ-RESOLVE-2 (shared switch) | 7 (loop gate) |
| REQ-RESOLVE-3 (failure → fallback) | 6 |
| REQ-RESOLVE-4 (parser = permanent fallback) | 7 |
| REQ-RESOLVE-5 (read-only, byte-identity) | 6 |
| REQ-RESOLVE-6 (no id/schema egress) | 2 |
| REQ-RESOLVE-7 (scope-context builder) | 2 |
| REQ-RESOLVE-8 (tool-use / emit_action schema) | 4 |
| REQ-RESOLVE-9 (request params) | 4 |
| REQ-RESOLVE-10 (8 s, no retry) | 6 |
| REQ-RESOLVE-11 (HttpTransport seam) | 6 |
| REQ-RESOLVE-12 (system prompt = ISA spec) | 3 |
| REQ-RESOLVE-13 (validation/mapping gate a–e) | 5 |
| REQ-RESOLVE-14 (new TU, lookupNoun hoist, no new deps) | 1, 2 |
| REQ-RESOLVE-15 (unit tests) | 2, 4, 5, 6, 7 (dispatch-fallback), 8 |
| REQ-RESOLVE-16 (gated live smoke) | 9 |
| REQ-PROTO-6b (tier-b passthrough) | 8 |

All 16 RESOLVE requirements plus the tier-b passthrough are covered. No step
introduces a new build dependency, a write from the resolver path, or an id on the wire.
