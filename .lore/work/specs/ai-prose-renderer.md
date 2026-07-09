---
title: AI prose renderer — requirements
date: 2026-07-06
status: implemented
tags: [ai-integration, prose-renderer, claude-api, render, fallback, grounding, requirements]
modules: [render, loop, prose]
related: [.lore/work/design/ai-prose-renderer.md, .lore/work/brainstorm/ai-integration-points.md, .lore/work/specs/engine-foundation-prototype.md, .lore/work/research/similar-projects-and-approaches.md, .lore/vision.md]
req-prefix: PROSE
---

# AI prose renderer — requirements

## Context

First AI feature in the engine. Replaces the dumb templates in `render.cpp` with Claude-generated narration while preserving the renderer contract established by the engine foundation (REQ-PROTO-9: renderer performs no writes; no output describes a state change lacking an event row). Design decisions are settled in [.lore/work/design/ai-prose-renderer.md]; this spec encodes them as verifiable requirements.

**Boundary declaration (vision principle 1):** action resolution, state mutation, exits/items listing — deterministic (engine). Narrative sentence construction — AI-driven, because it is flavor with no mechanical consequence (vision tension table: the one place AI prose freedom wins).

## Requirements

### Dispatch & fallback

**REQ-PROSE-1** — When AI narration is enabled, each ticked turn's output is produced by `aiRender(db, turn)`; when it returns no value, the existing template `render(db, turn)` produces the output instead. A turn never fails to produce output because of the AI path.

**REQ-PROSE-2** — AI narration is enabled iff the `ANTHROPIC_API_KEY` environment variable is set (non-empty) and the value of `TEXTWORLD_AI` is not exactly the string `0` (unset or any other value counts as enabled). When disabled, the game prints a single startup notice ("AI narration off — template mode"), never attempts an AI call, and behaves exactly as the pre-AI build.

**REQ-PROSE-3** — Every failure of an attempted AI call — HTTP error, timeout, refusal, malformed/unparseable response, validation-gate failure — results in silent per-turn fallback to templates. No failure crashes, blocks past the timeout, or leaks an error into player-visible prose (a single non-prose diagnostic line on stderr is permitted). (The no-key case is not a call failure; it is disabled mode per REQ-PROSE-2.)

**REQ-PROSE-4** — `render.cpp` templates remain intact and untouched as the permanent fallback path. `renderError` (tier-a parse failures, no event row) and the startup courtesy render (`renderRoomOf`, no tick) remain template-only.

### Contract preservation

**REQ-PROSE-5** — The AI render path performs only SELECT statements against the database. REQ-PROTO-9 is preserved: no write, and no output line describing a state change that lacks a sourcing event row. The existing db-byte-identical-across-render test discipline extends to `aiRender`.

**REQ-PROSE-6** — Nothing beyond the facts payload's specific fields (REQ-PROSE-7) and the fixed system prompt leaves the process: no other tables, no entity/row ids, no file paths, no full event history, no schema.

### Facts payload

**REQ-PROSE-7** — A pure function of `(db, turn)` builds a JSON facts payload containing exactly: this turn's event rows; the current room slice (name, canon description, exits, item names); the actor's inventory names; and the last ≤6 event rows preceding this turn. Nothing else. The builder is callable and unit-testable without any network dependency.

### API request

**REQ-PROSE-8** — Requests go to the Anthropic Messages API (`POST /v1/messages`, `anthropic-version: 2023-06-01`) with model `claude-opus-4-8` by default, overridable via `TEXTWORLD_MODEL`. `max_tokens` 1024; no `thinking` parameter; no streaming; no prompt caching.

**REQ-PROSE-9** — The HTTP call has a total timeout of 8 seconds and performs no retries. Timeout counts as failure per REQ-PROSE-3.

**REQ-PROSE-10** — The HTTP transport sits behind an injectable seam so tests can substitute canned responses without network access.

### Grounding & prompt rules

**REQ-PROSE-11** — The system prompt instructs the model: narrate only supplied events; atmosphere (light, air, sound) is permitted but no noun/object absent from the facts may be introduced; never contradict a fact; include `canon_description` verbatim when present; 1–4 sentences per event; plain text, no markdown, no meta-commentary, final answer only.

**REQ-PROSE-12** — Canon room prose is never re-generated or paraphrased in player-visible output: on turns containing a room-describing event, the canon description appears verbatim (see validation gate, REQ-PROSE-13). **Definition:** a *room-describing event* is a `moved` event, or a `looked` event whose `detail` column is NULL (a `looked` event with `detail = 'inventory'` is an inventory listing, not a room description — this distinction currently lives only in `render.cpp`'s dispatch and is made normative here).

**REQ-PROSE-12b** — `failed` event detail text (engine-authored refusals such as "You can't go that way.") is likewise protected: it appears verbatim in the output. The model may add atmosphere around it but may not paraphrase it — paraphrase risks inventing affordances (e.g. rewording a wall-bump as "the door is locked").

### Validation gate (mechanical, pre-display)

**REQ-PROSE-13** — AI output is displayed only if all of the following hold; otherwise the turn falls back to templates:
  a. HTTP 200 and `stop_reason == "end_turn"`;
  b. the response body parses as valid JSON containing a non-empty text block (a malformed/unparseable response fails this clause);
  c. if the turn includes a room-describing event (REQ-PROSE-12), `canon_description` is present as an exact substring;
  d. if the turn includes a `failed` event, its detail text is present as an exact substring (REQ-PROSE-12b);
  e. total AI prose length ≤ 1200 characters.

**REQ-PROSE-14** — Exits, visible-items ("You see: …"), and inventory listings are always emitted deterministically by the engine, appended after the AI prose. They are never delegated to the model.

### Build

**REQ-PROSE-15** — Build gains system libcurl (`find_package(CURL REQUIRED)`) and a vendored nlohmann/json single header (`vendor/json.hpp`). On macOS the project still builds with nothing to install.

### Testing

**REQ-PROSE-16** — Unit tests (no network, run in default `tests` target) cover: facts-payload builder against the seeded world; validation gate (each failure clause of REQ-PROSE-13); fallback dispatch on `nullopt`; request-body assembly against the REQ-PROSE-8 parameters; deterministic append of exits/You-see/inventory lines after a canned successful AI response (REQ-PROSE-14); db byte-identity across `aiRender` with a fake transport.

**REQ-PROSE-17** — A live end-to-end smoke test exists but runs only when `TEXTWORLD_AI_LIVE_TEST=1`; it is skipped in the default test run. If/when CI is added, it must remain excluded there.

## Out of scope (explicit deferrals)

- **Noun-invention checker** — v1 relies on the REQ-PROSE-11 prompt rule plus playtesting. A mechanical checker is v2.
- **"Canon by extraction"** (research doc's proposal to write AI-invented nouns back as scenery rows) — deferred; as proposed it would have the render path writing, violating REQ-PROSE-5. Revisit only as a separate, explicitly write-side feature.
- **NL action resolver** — the fixed-verb parser stays; it is integration point #1, a separate step.
- **Async/streaming rendering, prompt caching, retries** — v2 latency levers.
- **AI on `renderError` / startup render** — template-only per REQ-PROSE-4.

## AI Validation

How the AI verifies completion, behaviorally. Each item names the requirements it verifies.

1. **Build check (REQ-PROSE-15):** `cmake -B build && cmake --build build` succeeds on macOS with no newly installed packages; `vendor/json.hpp` present; `CMakeLists.txt` uses `find_package(CURL REQUIRED)`.
2. **Contract grep (REQ-PROSE-5):** `grep -En "INSERT|UPDATE|DELETE" src/prose.cpp` returns nothing (same discipline as `render.cpp`'s header comment).
3. **Prompt-content inspection (REQ-PROSE-11):** read the system-prompt constant in `src/prose.cpp` and confirm it contains each required rule: events-only narration, no-new-nouns/atmosphere-permitted, no fact contradiction, canon verbatim, 1–4 sentences, plain text / no meta / final-answer-only.
4. **Fallback run (REQ-PROSE-1, -2, -4):** with `ANTHROPIC_API_KEY` unset, run `./build/textworld` in a scratch dir; issue `look`, `take key`, `inventory`, an invalid direction, `quit`; output matches pre-AI template behavior and the startup notice appears exactly once.
5. **Kill-switch (REQ-PROSE-2):** with a key set but `TEXTWORLD_AI=0`, behavior matches check 4.
6. **Unit suite (REQ-PROSE-7, -8, -10, -13, -14, -16):** `./build/tests` passes, including new tests for: facts-builder payload fields (exactly the REQ-PROSE-7 set, nothing more — also covers REQ-PROSE-6); each REQ-PROSE-13 clause a–e; dispatch fallback on `nullopt`; request body containing `claude-opus-4-8` (and the `TEXTWORLD_MODEL` override), `max_tokens: 1024`, no `thinking`/`stream` keys; deterministic exits/You-see/inventory lines appended after canned successful prose; db byte-identity across `aiRender` with a fake transport.
7. **Timeout behavior (REQ-PROSE-3, -9):** using the fake transport's timeout case (or an unreachable endpoint), confirm the turn renders via templates within the timeout bound, no crash, nothing AI-flavored in player output.
8. **Live smoke (REQ-PROSE-12, -12b, -14, -17 — manual, optional):** with a real key and `TEXTWORLD_AI_LIVE_TEST=1`, play `go north`, `take lantern`, `look`, `drop lantern`, plus one impossible action (e.g. `go up`); observe (a) prose varies from templates, (b) canon description verbatim on `look`/`go`, (c) `failed` detail text verbatim on the impossible action, (d) exits/You-see lines present and template-formatted, (e) turn latency under ~8 s or falls back.
9. **Invented-noun probe (REQ-PROSE-11 grounding, manual — the design's top risk):** during the live smoke, issue references to objects that do not exist (`take window`, `look` in a sparse room and check the prose). Confirm rendered prose never affirms a nonexistent object; parser will reject the verbs, but the preceding turns' prose must not have introduced the nouns.
