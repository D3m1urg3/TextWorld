---
title: AI prose renderer — LLM narration over the event log
date: 2026-07-06
status: implemented
tags: [ai-integration, prose-renderer, claude-api, llm, render, fallback, grounding]
modules: [render, loop]
related: [.lore/work/brainstorm/ai-integration-points.md, .lore/vision.md]
---

# AI prose renderer — LLM narration over the event log

## Goal

Replace the dumb templates in `render.cpp` with Claude-generated prose, without breaking the renderer contract (`render.hpp:7`): read-only, every output claim sourced by a fact. First AI feature in the engine; templates remain as permanent fallback.

## Decisions (settled with user)

| Decision | Choice | Rationale |
|---|---|---|
| Model | `claude-opus-4-8` | Best prose + rule-following (nouns-must-exist). Latency ~2–5 s/turn accepted for prototype. `TEXTWORLD_MODEL` env override kept cheap for later A/B. |
| HTTP client | System **libcurl** + vendored **nlohmann/json** single header | libcurl ships with macOS (`find_package(CURL)`); single-header JSON matches the sqlite3.c amalgamation precedent. Still "nothing to install." |
| Call style | Synchronous, blocking, post-commit | One turn = one call. 8 s timeout → template fallback. Async is premature for a prototype. |
| Thinking | Omitted (runs without thinking on Opus 4.8) | Flavor prose isn't reasoning-bound; latency matters. System prompt carries a final-answer-only instruction because Opus 4.8 without thinking can leak reasoning into visible text. |
| Streaming | No | Output ≤ ~150 tokens; `max_tokens: 1024`. |
| Prompt caching | Not used in v1 | Opus 4.8 minimum cacheable prefix is 4096 tokens; the narrator system prompt will be far smaller. Revisit if the prompt grows. |

## Architecture

```
loop.cpp runTurn()
  └─ after commit:
      aiRender(db, turn)  ──── std::optional<std::string>
        │  builds facts JSON (SELECTs only)
        │  POST /v1/messages via libcurl
        │  validates output (see Validation)
        ├─ ok        → return prose (+ deterministic Exits/You-see lines)
        └─ any fail  → std::nullopt → render(db, turn)  [existing templates]
```

New translation unit `src/prose.cpp` / `prose.hpp`:

- `std::optional<std::string> aiRender(Db& db, int64_t turn);`
- Reuses/extends `render.cpp`'s read-only lookups to assemble facts.
- **Contract note:** the unit performs only SELECTs against the DB. It adds a new effect category — network egress — which the original contract didn't anticipate. Documented in the header: *reads world state, writes nothing, sends facts (not the DB) to the LLM.*
- `render.cpp` is untouched. `loop.cpp` gains the dispatch (3 lines).

### Enable/disable

- AI active iff `ANTHROPIC_API_KEY` is set and `TEXTWORLD_AI` ≠ `0`. Otherwise templates, with a single startup notice ("AI narration off — template mode").
- No config file; env vars only. Fits prototype.

## Facts payload

Per turn, one user message containing structured facts:

```json
{
  "events": [{"verb": "took", "subject": "brass lantern"}],
  "room": {
    "name": "walled garden",
    "canon_description": "<prose from description table, verbatim>",
    "exits": ["north", "south"],
    "items": ["rusty iron key"]
  },
  "inventory": ["brass lantern"],
  "recent_events": [{"turn": 40, "verb": "moved"}, {"turn": 41, "verb": "took", "subject": "brass lantern"}]
}
```

- `recent_events`: last ~6 event rows from the `events` table — the DB *is* the transcript, so continuity ("Again?" on take/drop/take) costs no new state. Narrow context per vision principle 4.
- Facts builder is a pure function of (db, turn) → JSON string; unit-testable without network.

## Prompt

System prompt (stable, versioned as a constant in `prose.cpp`):

- Narrator role + style anchor (second person, present tense, restrained, no purple prose).
- **Grounding rules:** narrate only the supplied events; you may add atmosphere (light, air, sound) but may NOT introduce nouns/objects not present in the facts; never contradict a fact.
- **Canon rule:** if the facts include `canon_description`, include it **verbatim, unmodified** in your output; write connective prose around it.
- Output rules: 1–4 sentences per event, plain text, no markdown, no meta-commentary, final answer only (no visible reasoning).

## Canon vs fresh split (from brainstorm, now enforced)

- **Canon room prose**: passed to the model, must appear verbatim in output — mechanically checked (substring). This lets the AI write a transitional sentence *around* the canon paragraph instead of the engine crudely concatenating.
- **Exits / You-see / inventory lines**: appended deterministically by the engine after the AI prose. Player-critical mechanics never depend on the model remembering to list them.
- **Action sentences** (take/drop/wait): fresh each turn; variety welcome.
- `renderError` (tier-a parse failures): stays template. No event row → no facts → nothing to ground on.

## Validation (mechanical, pre-display)

Any failure → discard AI output, fall back to templates for that turn:

1. HTTP 200 and `stop_reason == "end_turn"` (refusal, max_tokens → fallback).
2. Non-empty text block (malformed/unparseable JSON fails here).
3. If the turn includes a `moved`/`looked` room event (`looked` with NULL detail): `canon_description` present as exact substring.
4. If the turn includes a `failed` event: its detail text present as exact substring — engine-authored refusals are protected like canon; paraphrase risks inventing affordances.
5. Length cap (~1200 chars) — guards rambling.

Noun-invention is **not** mechanically checkable in v1 — mitigated by the prompt rule and playtesting. A checker pass (second cheap model call or noun-extraction heuristic) is a possible v2, not now.

## Request shape (wire)

```json
POST https://api.anthropic.com/v1/messages
x-api-key: $ANTHROPIC_API_KEY
anthropic-version: 2023-06-01

{
  "model": "claude-opus-4-8",
  "max_tokens": 1024,
  "system": "<narrator prompt>",
  "messages": [{"role": "user", "content": "<facts JSON>"}]
}
```

Parse with vendored nlohmann/json; extract first `content` block with `type == "text"`. libcurl: 8 s total timeout, no retries in v1 (a slow turn is worse than a template turn).

## Cost & latency envelope

~600 input + ~120 output tokens per turn on Opus 4.8 ≈ **$0.006/turn**; a 500-turn playthrough ≈ $3. Latency 2–5 s per turn, blocking. Acceptable for prototype; `TEXTWORLD_MODEL=claude-haiku-4-5` exists as the escape hatch if it grates.

## Build changes

- `CMakeLists.txt`: `find_package(CURL REQUIRED)`, link `CURL::libcurl` to `textworld` (not to `tests` unless needed).
- `vendor/json.hpp`: nlohmann/json single-header amalgamation.

## Testing

- **Unit (no network):** facts-JSON builder against seeded world; canon-substring validator; fallback dispatch when `aiRender` returns nullopt; request-body assembly.
- **Fake transport:** `aiRender` takes the HTTP call through a small function seam so tests can inject canned API responses (success, refusal, malformed JSON, timeout).
- **Live smoke test:** manual, behind `TEXTWORLD_AI_LIVE_TEST=1`, skipped by default. Not in CI.
- **Playtest checklist:** invented-noun spotting ("look through window" probe), canon verbatim on revisit, repeat-action variety.

## Risks

| Risk | Mitigation |
|---|---|
| Invented affordances (nouns not in DB) | Prompt rule; playtest probe; v2 checker if it leaks |
| Canon mutation | Mechanical substring check → fallback |
| Latency ruins game feel | Blocking accepted for prototype; haiku override; async is the v2 lever |
| API down / no key | Silent template fallback; game never breaks |
| Voice drift across turns | Style anchor in system prompt; story seed (#3) later feeds tone |

## Decision

Build `prose.cpp` as described: Opus 4.8 via libcurl + vendored nlohmann/json, synchronous post-commit call, facts-JSON grounding with canon-verbatim substring validation, template fallback on every failure path. `render.cpp` untouched as the permanent fallback skeleton.
