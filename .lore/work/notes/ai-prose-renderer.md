---
title: "Implementation notes: AI prose renderer"
date: 2026-07-06
status: complete
tags: [implementation, notes, ai-integration, prose-renderer, claude-api]
source: .lore/work/plans/ai-prose-renderer.md
modules: [prose, render, loop]
related: [.lore/work/specs/ai-prose-renderer.md, .lore/work/design/ai-prose-renderer.md]
---

# Implementation notes: AI prose renderer

Orchestrated implementation of the 8-step approved plan. Each step: implement → test → review → commit, gate must pass before next step.

## Progress

- [x] Step 1 — Vendor json.hpp + CMake changes (REQ-PROSE-15) — commit `8a883d7`
- [x] Step 2 — Facts builder (REQ-PROSE-6, -7) — commits `e6539a7` + shape-pin follow-up
- [x] Step 3 — Transport seam + fake (REQ-PROSE-9, -10) — commit `ae2696d`
- [x] Step 4 — Validation gate (REQ-PROSE-13 a–e) — commit `9527a94` + never-throw pin follow-up
- [x] Step 5 — Prompt constant + request assembly (REQ-PROSE-8, -11) — commit `2f74070` + prompt-wording follow-up
- [x] Step 6 — aiRender assembly + dispatch wiring (REQ-PROSE-1..5, -12, -14) — commit `c6240a2`
- [x] Step 7 — Test completeness pass + live smoke (REQ-PROSE-16, -17) — commit `bba187e` + defect-fix follow-up
- [x] Step 8 — Final validation against spec (AI Validation items 1–7) — all PASS

## Log

### Session start (2026-07-06)

- lore-researcher: full artifact chain found (brainstorm resolved → spec draft → design draft → plan approved). No prior implementation work; no task files; no `.lore/learned/`.
- Retro lessons carried in: two deliberately-unfixed nits (tier-b refusal ordering, room block lists all portables) must NOT be fixed in passing; clangd may show false diagnostics — trust real builds; spec authoritative if docs disagree.
- Environment checks: network egress available (github reachable) → step 1 download feasible. `vendor/` exists with sqlite3 amalgamation only. Agents: no `.lore/lore-agents.md` → `general-purpose` fallback for implementation/testing/review roles.
- First step-1 dispatch killed by session limit before doing anything; re-dispatched clean.

### Step 1 (2026-07-06) — complete, commit `8a883d7`

- vendor/json.hpp = nlohmann/json v3.12.0 single-header; reviewer verified sha256 byte-identical to upstream release asset.
- CMake: `find_package(CURL REQUIRED)`; prose.cpp in `twcore`; CURL linked to twcore; `target_include_directories(twcore PRIVATE vendor)` matching sqlite precedent (bare `#include "json.hpp"`).
- **Deviation (accepted):** plan claimed `CMAKE_EXPORT_COMPILE_COMMANDS` already in CMakeLists — it lived only in build cache (`-D` at original configure). Agent added `set(CMAKE_EXPORT_COMPILE_COMMANDS ON)` so fresh build dirs keep regenerating; serves plan intent, kept.
- Test agent: fresh build dir clean, 324 checks pass, compile_commands regenerates. Noted stale schema_version-999999 fixture in $TMPDIR — pre-existing test scenario behavior, not ours.
- Review: clean apart from two instructed items (compile-proof `#include "json.hpp"` in stub; the CMAKE_EXPORT line). Both intentional.

### Step 2 (2026-07-06) — complete, commit `e6539a7` (+ test follow-up)

- `TurnFacts { payload, canonRequired, canonText, failedDetails }`, `buildFacts(Db&, int64_t)` exposed for tests. Fresh SELECTs only; events SELECT never reads the `object` column, so destination/container row ids can't enter the payload. Zero-subject and NULL-detail omitted; `"something"` placeholder unreachable.
- recent_events: `turn < ?`, `ORDER BY id DESC LIMIT 6`, reversed to chronological. Room slice = post-commit `location.container` (destination on moved — matches render.cpp:110 semantics).
- Tests reach json.hpp via `#include "../vendor/json.hpp"` (vendor include dir is PRIVATE to twcore; CMake untouched — correct call).
- Gate 2 verified by test agent incl. mutation check (broken assert → nonzero exit → restored). 1139 checks green. SELECT-grep empty.
- Review finding fixed in follow-up commit: recent_events entries had no exact key-set assertion — numeric id leak would have passed the string-only hygiene sweep.
- **Informational, not fixed (unreachable in seeded world):** room lacking a `description` row → `canonRequired=true` with empty canonText → REQ-PROSE-13c would vacuously pass; room lacking `name` row silently drops the key. Noted for a future retro; fixing = scope creep now.
- Implementer gotcha worth remembering: range-for over `json::parse(...)["recent_events"]` iterates a destroyed temporary (zero iterations, silent); bind parsed json to a named variable first. Caught because check count didn't rise.

### Step 3 (2026-07-06) — complete, commit `ae2696d`

- Seam exactly per plan (`HttpResponse`/`HttpTransport` in prose.hpp); production `curlTransport` in anon namespace: POST /v1/messages, CURLOPT_TIMEOUT 8L, headers fixed, key via getenv at call time only, cleanup on all paths, POSTFIELDS lifetime safe. Three-arg `aiRender` calls transport exactly once, returns nullopt for now; two-arg delegates.
- Gate verified incl. mutation check; 1348 checks; `env -u ANTHROPIC_API_KEY ./build/tests` green; curlTransport unreachable from tests (only ref = two-arg overload). No secret literals in repo.
- Review advisories (non-blocking, recorded): (1) unset key → libcurl DROPS the empty-value `x-api-key` header entirely (would need `"x-api-key;"` to send blank); safe, and unreachable once step-6 enable-check lands. (2) no `curl_global_init` — implicit init in `curl_easy_init`, fine single-threaded, latent race only if threads ever appear.
- Expected churn: the 200-status test asserts nullopt with a "validation later" comment — will be updated in step 4/6.

### Step 4 (2026-07-06) — complete, commit `9527a94` (+ test follow-up)

- `validateAiResponse(const HttpResponse&, const TurnFacts&)` — anchors kept on TurnFacts, no sub-struct. Parse once via `allow_exceptions=false`; all accesses type-guarded; eval order status→parse→stop_reason→content[0]→c→d→e, diagnostics reported in a→e display order; single stderr line `aiRender: response rejected, clause <x> failed: <why>`.
- Judgment call (reviewed, accepted): transportError/status-0 fails clause a (spec defines gate purely as a–e); diagnostic wording accurate.
- First-block semantics pinned: content[0] must be text — tool_use first + text later still rejected.
- Test agent adversarial probe: hostile type-confused bodies all degrade to nullopt, no abort. Mutation check killed. 1364 checks.
- Review finding fixed in follow-up commit: never-throw guards weren't test-pinned — added type-confusion fixtures.
- Consistent with step-2 note: empty canonText passes clause c vacuously (`find("")==0`) — same unreachable-in-seed edge, still not fixed by design.

### Step 5 (2026-07-07) — complete, commit `2f74070` (+ prompt follow-up)

- Session limit cut the first attempt mid-read (only the prose.hpp declaration landed); resumed same agent from partial state, finished cleanly.
- `kSystemPrompt` (anon namespace) with all REQ-PROSE-11 + 12b rules; explains the facts JSON input format; recent_events marked already-narrated. `buildRequestBody`: TEXTWORLD_MODEL (non-empty) else `claude-opus-4-8`; max_tokens 1024; `j.size()==4` tripwire pins absence of thinking/stream/cache keys.
- `ScopedModelEnv` RAII guard handles was-unset → unsetenv restore. Suite green with model env unset/set/empty. Mutation check: default-model mutant killed by two independent asserts. 1398 checks.
- Implementer judgment (accepted — matches design wording verbatim): prompt says canon "if present, include verbatim" while buildFacts emits canon_description every turn → model includes canon every turn. Extra inclusion can't fail the gate; omission on room-describing turns can. Errs safe. Possible prose-repetitiveness observation for live smoke/retro.
- Review finding fixed in follow-up: atmosphere carve-out internal tension (permission for light/air/sound retracted by absolute no-new-nouns sentence — literal model might suppress atmosphere; tension exists in REQ-PROSE-11 wording itself). Reworded: ambient qualities are the ONLY beyond-facts evocables. Not a spec divergence — same intent, clearer.

### Step 6 (2026-07-07) — complete, commit `c6240a2`

- Full `aiRender(db,turn,transport)`: buildFacts→buildRequestBody→transport→validateAiResponse→prose + "\n" + deterministic tail. Whole body try/catch (std::exception AND `...`), any failure → one stderr line → nullopt. Deterministic appends from prose.cpp's own fresh SELECTs, character-identical to render.cpp (proven by comparing against `render()`'s own tail, not hand-typed). `aiNarrationEnabled()` added. loop.cpp 3-line dispatch exactly per plan. main.cpp notice once before startup render when disabled.
- **Hermeticity fix (important, and correct):** tests main() now saves+unsets ANTHROPIC_API_KEY and TEXTWORLD_AI suite-wide — otherwise a developer shell with a real key would route every pre-existing runTurn test through live curl. Verified: `ANTHROPIC_API_KEY=dummy ./build/tests` still instant/green.
- Both verifiers cut off first attempt (Fable 5 limit); switched to Opus 4.8; re-ran fresh, both clean. 1454 checks, mutation check bites, manual runs confirm notice-exactly-once + template-identical gameplay (key-unset and TEXTWORLD_AI=0), and enabled-with-garbage-key → 401 → template fallback in 0.6s (≪8s bound), no crash.
- **Known coverage gap (accepted):** the 2 loop.cpp glue lines `return {Ticked, *prose}` aren't exercised end-to-end — runTurn hardwires curlTransport (no injectable seam by design), so driving them needs network. aiRender happy path is covered directly via fake transport; step-7 live smoke exercises the glue with a real key. A regression returning render() instead of *prose inside that block wouldn't be caught by the default suite. Recorded for retro.
- Stale-comment cleanup: the old step-3 200-response transport test kept passing coincidentally (its canned body lacks stop_reason → now fails clause a); comment updated.

### Step 7 (2026-07-07) — complete, commit `bba187e` (+ defect-fix follow-up)

- Audit: REQ-PROSE-16 items 1–6 all already genuinely covered (spot-checked by review, not overstated). Item 7 (timeout→template, nothing AI-flavored) was the only gap.
- Live smoke `testProseLiveSmoke`: no-op unless `TEXTWORLD_AI_LIVE_TEST=="1"` (first statement); invoked as first line of main() BEFORE the hermetic unsetenv so a real key survives; loud skip (not crash) if requested without a key. Drives one real turn via 2-arg production aiRender. Verified skipped by default, no network.
- **Two review defects, both fixed in follow-up commit:**
  1. Item-7 "fallback == template" assertion was a tautology — `shown = out ? *out : render(db,1)` after asserting `!out.has_value()` reduces to `render==render`, never bites. The one bit of new coverage the step claimed. Removed; real fallback byte-identity is the AI-off runTurn test. Kept nullopt + one-call + no-crash.
  2. Live-smoke `CHECK(out.has_value())` coupled to validation passing (canon-verbatim / stop_reason / length) — model-dependent, flaky. Replaced with dispatch-mirror `shown = out ? *out : render(db,turn); CHECK(!shown.empty()); CHECK(shown.find("Exits:")...)` — mechanical-only per REQ-PROSE-17.
- Note: the pre-existing refusal test at ~1313 has the same vacuous ternary pattern but predates step 7 — left as-is (not in scope; candidate for retro/simplify).
- Defect-2 fix (live-smoke dispatch-mirror) was completed inline by the orchestrator after the fix agent hit a session limit mid-edit; a fresh short agent built+gated+committed it (`b32c603`, 1456 checks).

### Step 8 (2026-07-07) — complete, all gating items PASS

Final validation from a fresh build dir, AI Validation items 1–7:
1. Build (REQ-PROSE-15): fresh `cmake -B build-val` clean, CURL 8.7.1 found, vendor/json.hpp present, `find_package(CURL REQUIRED)` in CMakeLists.
2. Contract grep (REQ-PROSE-5): prose.cpp INSERT/UPDATE/DELETE → empty.
3. Prompt (REQ-PROSE-11): kSystemPrompt carries every rule incl. 12b failed-detail-verbatim and the atmosphere carve-out.
4. Fallback run (REQ-PROSE-1/2/4): key unset → template gameplay, notice exactly once.
5. Kill-switch (REQ-PROSE-2): dummy key + TEXTWORLD_AI=0 → transcript identical to item 4.
6. Unit suite (REQ-PROSE-7/8/10/13/14/16): 1456 checks exit 0; hermetic with dummy key set.
7. Timeout (REQ-PROSE-3/9): dummy key + AI=1 → 401 → template fallback, no crash, 0.63s ≪ 8s bound.

Items 8–9 (live play-through smoke, invented-noun probe) deferred to user — need a real paid ANTHROPIC_API_KEY.

## Summary

**Built:** the AI prose renderer — LLM-generated second-person narration over the event log, grounded by a facts payload, mechanically validated, with template fallback on every failure path. New TU `src/prose.cpp`/`.hpp` (facts builder, libcurl transport seam, validation gate, narrator prompt, request builder, `aiRender`, enable check); 3-line dispatch in `loop.cpp`; startup notice in `main.cpp`. `render.cpp`/`.hpp` untouched as permanent fallback.

**8 steps, all gated and passed.** Commits: `8a883d7` (vendor+cmake), `e6539a7`+`2fb0152` (facts builder), `ae2696d` (transport seam), `9527a94`+`9e78675` (validation gate), `2f74070`+`2a4d034` (prompt+request), `c6240a2` (aiRender+dispatch), `bba187e`+`b32c603` (test completeness+live smoke). One granular commit per step + focused review-fix follow-ups.

**Divergences from plan (all minor, none required user authorization):** (1) added `CMAKE_EXPORT_COMPILE_COMMANDS` to CMakeLists — plan assumed it was already there; (2) suite-wide env unset in tests main() for hermeticity — necessary so a developer's real key can't route existing runTurn tests through live curl; serves REQ-PROSE-15 intent.

**Known coverage gap (recorded, accepted):** the 2 loop.cpp dispatch glue lines (`return {Ticked, *prose}`) aren't exercised end-to-end — runTurn hardwires curlTransport with no injectable seam by design. aiRender's happy path is covered directly via fake transport; the gated live smoke exercises the glue with a real key.

**Deferred per scope fence (unchanged):** noun-invention checker, canon-by-extraction, NL resolver, async/caching/retries, AI on renderError/startup. Two known foundation-retro nits left unfixed by design.

**Open for the user:** run AI Validation items 8–9 (live play-through + invented-noun probe) with a real key and `TEXTWORLD_AI_LIVE_TEST=1`.
