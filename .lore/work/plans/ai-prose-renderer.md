---
title: "Implementation plan: AI prose renderer"
date: 2026-07-06
status: executed
tags: [plan, ai-integration, prose-renderer, claude-api, libcurl, render, fallback]
modules: [prose, render, loop]
related: [.lore/work/specs/ai-prose-renderer.md, .lore/work/design/ai-prose-renderer.md, .lore/work/brainstorm/ai-integration-points.md, .lore/work/specs/engine-foundation-prototype.md, .lore/vision.md]
---

# Implementation plan: AI prose renderer

**Source spec:** `.lore/work/specs/ai-prose-renderer.md` (REQ-PROSE-1 … REQ-PROSE-17, incl. 12b; AI Validation items 1–9)
**Source design:** `.lore/work/design/ai-prose-renderer.md` (all decisions settled — **do not re-litigate any of them during implementation**)
**Background (read-only, if needed):** `.lore/work/brainstorm/ai-integration-points.md`, `.lore/work/specs/engine-foundation-prototype.md` (REQ-PROTO-9 renderer purity must be preserved), `.lore/vision.md`

A fresh session needs nothing beyond the documents above and this plan.

## Settled decisions (locked; the design records the rationale)

| Area | Decision |
|---|---|
| Model | `claude-opus-4-8` default; `TEXTWORLD_MODEL` env override. No `thinking`, no streaming, no prompt caching. `max_tokens` 1024. |
| Transport | System libcurl (`find_package(CURL REQUIRED)`) + vendored nlohmann/json single header at `vendor/json.hpp`. Nothing to install on macOS. |
| Shape | New TU `src/prose.cpp` / `src/prose.hpp`. `std::optional<std::string> aiRender(Db&, int64_t turn);` SELECTs only. Synchronous post-commit call from `loop.cpp`. 8 s total timeout, no retries. Any failure → `std::nullopt` → existing template `render()`. `render.cpp` stays untouched as the permanent fallback. |
| Enable | AI active iff `ANTHROPIC_API_KEY` set (non-empty) and `TEXTWORLD_AI` is not exactly the string `"0"`. When off: single startup notice "AI narration off — template mode". |
| Grounding | Mechanical validation gate before display (REQ-PROSE-13 a–e), incl. `canon_description` and `failed`-detail verbatim substring checks. Exits / You-see / inventory lines appended deterministically by the engine, never by the model. |
| Testing | HTTP behind an injectable seam; unit tests with a fake transport in the default `tests` target; live smoke only behind `TEXTWORLD_AI_LIVE_TEST=1`. |

## Scope fence (explicit deferrals — do not build)

- Noun-invention checker (v1 = prompt rule + playtest; checker is v2).
- "Canon by extraction" (write-side; would violate REQ-PROSE-5).
- NL action resolver (integration point #1, separate step; fixed parser stays).
- Async/streaming rendering, prompt caching, retries.
- AI on `renderError` or the startup courtesy render — both stay template-only (REQ-PROSE-4).

## Where changes land

- `CMakeLists.txt` — CURL, new source in `twcore` (24 lines today).
- `vendor/json.hpp` — new, vendored single header.
- `src/prose.hpp`, `src/prose.cpp` — new TU; everything AI lives here.
- `src/loop.cpp` — `runTurn` line 56 (`return {TurnOutcome::Ticked, render(db, currentTurn(db))};`) grows the ~3-line dispatch.
- `src/main.cpp` — startup notice when AI is off.
- `tests/tests.cpp` — new test functions; existing micro-harness (`CHECK`, `TempDbFile`, `readFileBytes`) is sufficient.
- **Not touched:** `src/render.cpp`, `src/render.hpp`, all other engine TUs, schema, seed.

## Step sequence

<svg viewBox="0 0 760 210" xmlns="http://www.w3.org/2000/svg" style="max-width:100%">
  <style>
    .box{fill:#eef2f7;stroke:#5b7a9d;rx:8}
    .gate{fill:#f9efef;stroke:#a05252;rx:8}
    .t{font:12px monospace;fill:#1a2733}
    .n{font:10px sans-serif;fill:#666}
    .a{stroke:#5b7a9d;stroke-width:1.5;marker-end:url(#ar)}
  </style>
  <defs><marker id="ar" markerWidth="8" markerHeight="8" refX="7" refY="3" orient="auto"><path d="M0,0 L8,3 L0,6 z" fill="#5b7a9d"/></marker></defs>
  <rect class="box" x="8" y="20" width="150" height="34"/><text class="t" x="18" y="41">1 vendor+cmake</text>
  <rect class="box" x="190" y="20" width="150" height="34"/><text class="t" x="200" y="41">2 facts builder</text>
  <rect class="box" x="372" y="20" width="160" height="34"/><text class="t" x="382" y="41">3 transport seam</text>
  <rect class="box" x="564" y="20" width="160" height="34"/><text class="t" x="574" y="41">4 validation gate</text>
  <rect class="box" x="8" y="90" width="170" height="34"/><text class="t" x="18" y="111">5 prompt+request</text>
  <rect class="box" x="210" y="90" width="170" height="34"/><text class="t" x="220" y="111">6 aiRender+dispatch</text>
  <rect class="box" x="412" y="90" width="150" height="34"/><text class="t" x="422" y="111">7 test pass</text>
  <rect class="gate" x="240" y="160" width="300" height="34"/><text class="t" x="252" y="181">8 VALIDATE against spec</text>
  <line class="a" x1="158" y1="37" x2="190" y2="37"/>
  <line class="a" x1="340" y1="37" x2="372" y2="37"/>
  <line class="a" x1="532" y1="37" x2="564" y2="37"/>
  <line class="a" x1="640" y1="54" x2="100" y2="90"/>
  <line class="a" x1="178" y1="107" x2="210" y2="107"/>
  <line class="a" x1="380" y1="107" x2="412" y2="107"/>
  <line class="a" x1="487" y1="124" x2="420" y2="160"/>
  <text class="n" x="8" y="150">strictly sequential; tests grow per step, step 7 is the completeness pass</text>
</svg>

Steps are strictly sequential — each is the smallest testable increment (vision principle 5) and its gate must pass before the next begins. Test cases are written as each capability lands; step 7 closes gaps against REQ-PROSE-16.

---

### Step 1 — Vendor json.hpp + CMake changes (REQ-PROSE-15)

**Files:** `vendor/json.hpp` (new), `CMakeLists.txt`, stub `src/prose.hpp` + `src/prose.cpp`.

- Download the nlohmann/json single-header amalgamation (`json.hpp` from the latest release at github.com/nlohmann/json) and commit it as `vendor/json.hpp`. **This is the plan's only network-dependent moment** — mirrors the sqlite3.c precedent (REQ-PROTO-1). If the implementing session has no network egress, ask the user to supply the file; do not substitute a package-managed dependency.
- `CMakeLists.txt`: `find_package(CURL REQUIRED)`; add `src/prose.cpp` to `twcore`; `target_link_libraries(twcore PRIVATE ... CURL::libcurl)`. Linking CURL to `twcore` (rather than only to `textworld`, as the design sketch had it) is a deliberate elaboration: `prose.cpp` must live in `twcore` so the `tests` target can exercise `aiRender` — the design's "not to tests unless needed" clause is triggered; it is needed.
- Stub `prose.hpp`/`prose.cpp`: header declares `std::optional<std::string> aiRender(Db& db, int64_t turn);` returning `std::nullopt` for now. Header comment documents the new effect category verbatim from the design: *reads world state, writes nothing, sends facts (not the DB) to the LLM* — plus the render contract inheritance (SELECTs only, no output claim without a sourcing event row).

<div style="border-left:4px solid #a05252;background:#f9efef;padding:6px 12px;margin:8px 0"><strong>Gate 1:</strong> <code>cmake -B build && cmake --build build</code> succeeds from clean on macOS with nothing newly installed; a translation unit including <code>vendor/json.hpp</code> compiles; <code>./build/tests</code> still exits 0.</div>

### Step 2 — Facts builder, pure and tested (REQ-PROSE-6, -7)

**Files:** `src/prose.cpp`, `src/prose.hpp`, `tests/tests.cpp`.

- A `TurnFacts` struct built by a pure function of `(db, turn)` — no network, no globals:
  - `payload` — the JSON string sent as the user message, containing **exactly** (REQ-PROSE-7): `events` (this turn's rows, name-resolved: verb, subject *name*, detail — **no entity/row ids**, per REQ-PROSE-6 and the design's wire example), `room` (`name`, `canon_description`, `exits`, `items` — names only), `inventory` (names), `recent_events` (last ≤6 event rows preceding this turn: turn number, verb, subject name).
  - **Zero-id events:** `systems.cpp` appends `looked`/`waited`/`failed` events with `subject=0, object=0`. A `subject`/`object` of `0` is **omitted** from the event entry — never resolved through a name lookup (which would yield the `"something"` placeholder and leak a meaningless noun into the grounding payload). Same rule for NULL `detail`: omit the key.
  - Validation anchors computed from the same SELECTs, for step 4's gate: `canonRequired` + the canon text (true iff the turn contains a *room-describing event*: verb `moved`, or `looked` with NULL `detail` — REQ-PROSE-12's normative definition), and the list of `failed` event detail texts (REQ-PROSE-12b).
  - The room slice is the **actor's current room** (post-commit state; for `moved` turns that is the destination, matching `render.cpp`'s `object` column usage).
- All lookups are fresh SELECTs in `prose.cpp` (`render.cpp`'s helpers are in an anonymous namespace and `render.cpp` is untouchable; small duplication is the accepted cost).
- Expose the builder in `prose.hpp` for tests (e.g. `TurnFacts buildFacts(Db&, int64_t turn);`).

<div style="border-left:4px solid #a05252;background:#f9efef;padding:6px 12px;margin:8px 0"><strong>Gate 2:</strong> unit tests against the seeded world: drive turns via <code>runTurn</code> (take, move, look, inventory, wall-bump), then parse the payload with nlohmann/json and assert — top-level keys are exactly the REQ-PROSE-7 set; a <code>moved</code>/<code>looked</code> turn carries the destination room's canon description verbatim; no payload string contains an entity id, file path, or table name; <code>recent_events</code> caps at 6; anchors correct for room-describing vs <code>failed</code> vs plain turns. No network anywhere.</div>

### Step 3 — Transport seam + fake (REQ-PROSE-9, -10, part of -15)

**Files:** `src/prose.cpp`, `src/prose.hpp`, `tests/tests.cpp`.

- Seam type in `prose.hpp`:
  ```cpp
  struct HttpResponse {
      bool transportError = false;  // timeout, connect failure, curl error
      long status = 0;
      std::string body;
  };
  using HttpTransport = std::function<HttpResponse(const std::string& body)>;
  ```
  URL, headers (`x-api-key`, `anthropic-version: 2023-06-01`, `content-type`), and timeout are fixed properties of the production transport, not seam parameters — tests only ever vary the response.
- Production transport in `prose.cpp`: libcurl easy handle, `POST https://api.anthropic.com/v1/messages`, `CURLOPT_TIMEOUT` 8 s total, **no retries**. The `x-api-key` header value is read from `ANTHROPIC_API_KEY` at call time (and appears nowhere else — not in the payload, logs, or fixtures). Any curl failure → `transportError = true`.
- `aiRender` gains a test-visible overload: `aiRender(Db&, int64_t turn, const HttpTransport&)`; the two-arg production version binds the libcurl transport.
- Fake transport in tests: a lambda returning canned `HttpResponse` values (success, non-200, refusal stop_reason, malformed JSON body, `transportError` for the timeout case).

<div style="border-left:4px solid #a05252;background:#f9efef;padding:6px 12px;margin:8px 0"><strong>Gate 3:</strong> tests: <code>aiRender</code> with a <code>transportError</code> fake returns <code>nullopt</code> without crashing; the fake observes exactly one call (no retries); default <code>tests</code> run makes no network access (all transports fake). Build still clean.</div>

### Step 4 — Validation gate (REQ-PROSE-13 a–e)

**Files:** `src/prose.cpp`, `src/prose.hpp`, `tests/tests.cpp`.

- A pure function, exposed for tests, e.g. `std::optional<std::string> validateAiResponse(const HttpResponse&, const TurnFacts& facts);` returning the extracted prose iff **all** clauses hold, `nullopt` otherwise (it consumes only the anchor fields of `TurnFacts`; a dedicated anchors sub-struct is fine if cleaner):
  - **a.** `status == 200` and response JSON `stop_reason == "end_turn"`;
  - **b.** body parses as JSON and contains a first content block with `type == "text"` and non-empty text (unparseable body fails here, caught via nlohmann/json exception or `parse(..., nullptr, false)`);
  - **c.** if `anchors.canonRequired`: canon description present as **exact substring** of the text;
  - **d.** every `failed` detail in the anchors present as **exact substring**;
  - **e.** text length ≤ 1200 characters.
- **Implementation note (exception safety):** parse the body **once, up front**, exception-safe (`nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false)` or try/catch). A parse failure fails clause **b**; clause **a**'s `stop_reason` is read only after that parse succeeds — the spec's a-before-b listing is the display-decision order, not the evaluation order. `validateAiResponse` must never throw: Gate 4's tests call it directly, outside `aiRender`'s step-6 try/catch, and a leaked JSON exception would abort the test binary instead of producing a clean `CHECK` failure.
- Report clauses in a→e order for the stderr diagnostic wording; first failure wins.

<div style="border-left:4px solid #a05252;background:#f9efef;padding:6px 12px;margin:8px 0"><strong>Gate 4:</strong> unit tests: one canned response per failure clause (non-200; <code>stop_reason=max_tokens</code>; malformed JSON; empty text block; canon missing; canon paraphrased-not-verbatim; failed detail missing; 1201-char text) each → <code>nullopt</code>; a fully-valid canned response → the prose text. Each REQ-PROSE-13 clause has at least one dedicated failing test.</div>

### Step 5 — Prompt constant + request assembly (REQ-PROSE-8, -11)

**Files:** `src/prose.cpp`, `src/prose.hpp`, `tests/tests.cpp`.

- System prompt as a named constant in `prose.cpp` (stable, versioned by git). Must contain every REQ-PROSE-11 rule, phrased per the design: narrator role + style anchor (second person, present tense, restrained); narrate **only** supplied events; atmosphere (light, air, sound) permitted but **no noun/object absent from the facts** may be introduced; never contradict a fact; if `canon_description` present, include it **verbatim, unmodified**; if a `failed` event's detail is present, include the detail text **verbatim** (may add atmosphere around it, never paraphrase — REQ-PROSE-12b); 1–4 sentences per event; plain text, no markdown, no meta-commentary, **final answer only** (Opus 4.8 without thinking can leak reasoning).
- Request-body builder, exposed for tests, e.g. `std::string buildRequestBody(const std::string& factsPayload);`: model = `TEXTWORLD_MODEL` env if set else `claude-opus-4-8`; `max_tokens: 1024`; `system` = the constant; one user message = the facts payload. **No** `thinking`, `stream`, or cache-control keys.

<div style="border-left:4px solid #a05252;background:#f9efef;padding:6px 12px;margin:8px 0"><strong>Gate 5:</strong> unit tests: parsed request body has <code>model == "claude-opus-4-8"</code> by default and honors a <code>TEXTWORLD_MODEL</code> override (setenv in test, restore after); <code>max_tokens == 1024</code>; keys <code>thinking</code>/<code>stream</code> absent; <code>system</code> contains each REQ-PROSE-11 rule (substring spot-checks: "verbatim", noun rule, "final answer", sentence count, no markdown).</div>

### Step 6 — aiRender assembly + dispatch wiring (REQ-PROSE-1, -2, -3, -4, -5, -12, -14)

**Files:** `src/prose.cpp`, `src/prose.hpp`, `src/loop.cpp`, `src/main.cpp`, `tests/tests.cpp`.

- `aiRender(db, turn, transport)` end-to-end: buildFacts → buildRequestBody → transport → validateAiResponse → on success, append the deterministic block, return the string; **any** failure or thrown exception → single stderr diagnostic line (never into player prose) → `nullopt`. Wrap the whole body in try/catch — REQ-PROSE-3 says no AI-path failure may crash a turn.
- **Deterministic appends (REQ-PROSE-14):** after the validated AI prose, `prose.cpp` appends, from its own SELECTs, in `render.cpp`'s exact formats: for each room-describing event, `Exits: …` and `You see: …` lines for the actor's room; for each `looked detail='inventory'` event, the `You are carrying…` line. Never delegated to the model; canon description itself is *inside* the AI prose (verbatim-checked), so the append block starts at Exits.
- Enable check in `prose.hpp`/`prose.cpp`: `bool aiNarrationEnabled();` — `ANTHROPIC_API_KEY` set and non-empty, and `TEXTWORLD_AI` unset-or-anything-except exactly `"0"` (exact string compare).
- `loop.cpp` dispatch (the design's ~3 lines), replacing line 56's return:
  ```cpp
  if (aiNarrationEnabled()) {
      if (auto prose = aiRender(db, currentTurn(db))) {
          return {TurnOutcome::Ticked, *prose};
      }
  }
  return {TurnOutcome::Ticked, render(db, currentTurn(db))};
  ```
  Tier-a (`renderError`) and the startup render paths are untouched (REQ-PROSE-4).
- `main.cpp`: before the first prompt, if `!aiNarrationEnabled()`, print exactly one notice line: `AI narration off — template mode`.

<div style="border-left:4px solid #a05252;background:#f9efef;padding:6px 12px;margin:8px 0"><strong>Gate 6:</strong> tests: with a canned-success fake, <code>aiRender</code> output = AI prose + template-format Exits/You-see lines on a moved turn, inventory line on an inventory turn; <code>aiRender</code> with fake transport leaves the world file <strong>byte-identical</strong> (<code>readFileBytes</code> before/after — extends the REQ-PROSE-5 discipline); with <code>ANTHROPIC_API_KEY</code> unset, <code>runTurn</code> output is byte-identical to pre-AI template output. Manual: run <code>./build/textworld</code> with no key — startup notice appears exactly once, gameplay identical to the pre-AI build; same with a key set but <code>TEXTWORLD_AI=0</code>. <code>grep -En "INSERT|UPDATE|DELETE" src/prose.cpp</code> → nothing.</div>

### Step 7 — Test completeness pass + live smoke (REQ-PROSE-16, -17)

**Files:** `tests/tests.cpp`.

Close gaps against the REQ-PROSE-16 checklist (most items landed in steps 2–6; verify each has a test and add what's missing):

1. Facts-builder payload fields — exactly the REQ-PROSE-7 set (gate 2).
2. Each validation clause a–e (gate 4).
3. Fallback dispatch on `nullopt` — fake failing transport → templates render the turn.
4. Request-body assembly per REQ-PROSE-8 (gate 5).
5. Deterministic appends after canned success (gate 6).
6. Db byte-identity across `aiRender` with fake transport (gate 6).
7. Timeout path: `transportError` fake → template output, no crash, nothing AI-flavored in player-visible text (AI Validation item 7).

Plus the live smoke (REQ-PROSE-17): one test function that **returns immediately unless `TEXTWORLD_AI_LIVE_TEST=1`** — with a real key, drives a real turn through the production transport and asserts only the mechanical invariants (output non-empty, Exits line present). Skipped in the default run; if CI is ever added it must stay excluded.

<div style="border-left:4px solid #a05252;background:#f9efef;padding:6px 12px;margin:8px 0"><strong>Gate 7:</strong> <code>./build/tests</code> exits 0 with no network access and no <code>ANTHROPIC_API_KEY</code> required; deliberately break one new assertion, confirm nonzero exit, restore. Live smoke verified skipped by default (run once without the env var, observe skip).</div>

### Step 8 — Final validation against the spec

Run the spec's **AI Validation** items 1–7 end to end, in order, from a clean build: build check, contract grep, prompt-content inspection, fallback run (no key), kill-switch run (`TEXTWORLD_AI=0`), full unit suite, timeout behavior. Items 8–9 (live smoke play-through, invented-noun probe) are manual and optional — offer them to the user with a real key rather than gating on them; record whether they were run.

Record any deviation against its REQ-PROSE number; every requirement passes or carries an explicit, user-approved deviation note before the spec is marked `implemented`.

<div style="border-left:4px solid #a05252;background:#f9efef;padding:6px 12px;margin:8px 0"><strong>Final gate:</strong> AI Validation items 1–7 verified. This gate blocks marking the spec <code>implemented</code> and this plan <code>executed</code>.</div>

---

## Notes for the implementer

- **`render.cpp` is untouchable.** Reimplement the small lookups (exits, portables, inventory, canon prose) in `prose.cpp` with fresh SELECTs; keep output formats character-identical to the templates (`Exits: a, b.`, `You see: a, b.`, `You are carrying: …` / `You are carrying nothing.`).
- **Write-path discipline extends:** `prose.cpp` joins `render.cpp` under the SELECT-only rule; the header carries the same "READ-ONLY BY CONTRACT" banner plus the new egress sentence.
- **Do not pre-solve deferrals** (scope fence above). In particular: no retry loop "while we're at it", no response caching, no noun checking.
- **Two known, deliberately-unfixed nits** from the foundation retro (tier-b refusal ordering leaks absent-object info; room block lists all portables) — do **not** fix them in passing.
- **Env vars in tests:** tests that set/unset `ANTHROPIC_API_KEY`, `TEXTWORLD_AI`, `TEXTWORLD_MODEL` must save and restore prior values; the suite must pass regardless of the developer's shell environment.
- **Secrets:** the API key goes only into the `x-api-key` header inside the production transport. Never into the facts payload, logs, test fixtures, or committed files.
- Repo is under git now — keep commits granular, one per step. `compile_commands.json` already exists; keep it regenerating (`CMAKE_EXPORT_COMPILE_COMMANDS` on).
- No specialized expertise flagged: C++20, libcurl easy API, nlohmann/json, CMake — single generalist range.
