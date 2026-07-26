---
title: Running TextWorld's AI features on a local model instead of the Anthropic API
date: 2026-07-18
status: active
tags: [local-llm, ollama, llama-cpp, lm-studio, anthropic-messages-api, tool-use, tool-choice, inference-server, apple-silicon, qwen3]
modules: [prose, nlresolve, architect]
related: [.lore/work/brainstorm/ai-integration-points.md, .lore/work/research/procedural-room-feel.md]
---

# Running TextWorld's AI features on a local model instead of the Anthropic API

Gathered 2026-07-18. The question: TextWorld's three AI features — prose narration
(`prose.cpp`), NL input resolution (`nlresolve.cpp`), and room generation
(`architect.cpp`) — all POST the **Anthropic Messages wire format** to a hard-coded
`https://api.anthropic.com/v1/messages` via libcurl. What are the current
(2025–2026) options for pointing them at a **local model** instead?

## Key findings

1. **The wire format is no longer the obstacle — it's now an asset.** Within the
   last year, the local-inference ecosystem adopted Anthropic's `/v1/messages`
   format natively. Three mainstream servers speak it out of the box:
   - **llama.cpp** (`llama-server`) — added January 2026 ([PR #17570](https://github.com/ggml-org/llama.cpp/pull/17570),
     announced on the [ggml-org HF blog](https://huggingface.co/blog/ggml-org/anthropic-messages-api-in-llamacpp)).
     `POST /v1/messages` and `/v1/messages/count_tokens`, tool use with
     `tool_use`/`tool_result` blocks, proper Anthropic SSE event types, vision,
     thinking. Internally translated to its OpenAI pipeline.
   - **Ollama** — since v0.14.0 ([docs](https://docs.ollama.com/api/anthropic-compatibility)),
     at `http://localhost:11434/v1/messages`. Tools, streaming, system prompts,
     base64 vision. API key accepted but not validated.
   - **LM Studio** — since 0.4.1 ([docs](https://lmstudio.ai/docs/developer/anthropic-compat)),
     at `http://localhost:1234/v1/messages`. Streaming, tools, **and
     `tool_choice`** (documented example uses `{"type":"any"}`).

2. **The single blocking code change is the hard-coded URL.** All three call
   sites pin `https://api.anthropic.com/v1/messages` in `curl_easy_setopt`
   (`prose.cpp:205`, `nlresolve.cpp:140`, `architect.cpp:104`). A
   `TEXTWORLD_API_URL` (or `ANTHROPIC_BASE_URL`-style) env override — mirroring
   the existing `TEXTWORLD_MODEL` override — is the entire integration surface.
   Everything else (request bodies, response parsing, validation gates, silent
   fallback) works unchanged against a compatible endpoint. The AI on/off switch
   would also need a tweak: today AI-enabled == `ANTHROPIC_API_KEY` set; local
   servers don't validate keys, so any dummy value works (`ollama`), or the
   switch could accept the URL override as an alternative enabler.

3. **`tool_choice` is the one real compatibility gap, and it splits the three
   features.** The architect **forces** its tool
   (`tool_choice: {"type":"tool","name":"create_room"}`, `architect.cpp:263`);
   the resolver uses `"auto"`; narration uses no tools.
   - **Ollama explicitly does not support `tool_choice`** ("forcing specific
     tool use or disabling tools" is listed as unsupported). The architect's
     gate treats "no create_room call" as a failure → wall, so under Ollama the
     architect would *degrade more often* but never corrupt state — the existing
     fallback absorbs it.
   - **LM Studio documents `tool_choice`** and is the safest first target for
     the architect path.
   - **llama.cpp**: tool use confirmed; `tool_choice` handling not explicitly
     documented (it maps to the OpenAI internal format, which does support
     forced function choice) — needs one empirical check. Tool use requires
     starting `llama-server` with `--jinja` and a tool-capable model per
     community reports.

4. **The project's failure model makes this a low-risk experiment.** Every AI
   path already falls back silently (templates / fixed-verb parser / wall) on
   any HTTP error, timeout, or gate failure. A local model that is slower or
   flakier than the API degrades gracefully rather than breaking the game. The
   8-second `CURLOPT_TIMEOUT` is the number to watch: an 8B model on Apple
   Silicon generating ~200–1000 tokens usually fits, but a cold model load
   (first request after server start) can blow it — pre-warm the model
   (`ollama run`, keep-alive) or accept a template-rendered first turn.

5. **Model choice (consumer Apple Silicon, mid-2026 consensus):**
   - **Qwen3 8B / 14B** — repeatedly cited as the most reliable small model for
     constrained tool calling; ~5.5 GB at Q4_K_M for 8B. Best single default
     for all three features. Newer **Qwen3.6** ranks top for tool reliability.
   - **GLM-4.7-flash** — popular pairing with llama.cpp's Anthropic endpoint
     for Claude-Code-style workloads.
   - **Gemma 3 12B / 27B** — strong prose voice; weaker/less consistent tool
     calling; a candidate for *narration only* if running per-feature models.
   - Small models will follow the strict "tool call only, no prose" system
     prompts less reliably than Opus — the existing validation gates
     (clause checks, schema-enforced enums) were built for exactly this and
     will catch drift; expect a higher rejection/fallback rate than with Claude.

6. **A translation proxy (LiteLLM) is no longer necessary but still useful.**
   LiteLLM's proxy accepts Anthropic-format `/v1/messages` and routes to any
   backend (including Ollama models by `ollama/<name>`), adding logging, model
   routing, and fallbacks-to-cloud. Worth it only if you want *hybrid* routing
   (e.g. local resolver, cloud architect) without touching C++ — otherwise the
   native endpoints make it an extra moving part.

## Options compared

| Option | Endpoint | Tools | Forced `tool_choice` | Effort in TextWorld | Notes |
|---|---|---|---|---|---|
| **LM Studio** | `localhost:1234/v1/messages` | ✅ | ✅ (documented) | URL override only | GUI app; easiest full-compat path; MLX-optimized on Apple Silicon |
| **llama.cpp `llama-server`** | `localhost:8080/v1/messages` | ✅ (`--jinja`) | likely (verify) | URL override only | Lightest/headless; brew-installable; the upstream everything else wraps |
| **Ollama ≥ 0.14** | `localhost:11434/v1/messages` | ✅ | ❌ | URL override only | Easiest model management; architect degrades to wall-fallback more often |
| **LiteLLM proxy** | configurable `/v1/messages` | ✅ | ✅ (translates) | URL override only | Adds hybrid local/cloud routing; extra service to run |
| **vLLM** | OpenAI format only | ✅ | via proxy | needs proxy | Server-class throughput; overkill for one player, no native Anthropic format |

## Recommended path

1. Add a `TEXTWORLD_API_URL` env override (default: the current Anthropic URL)
   to the three `CURLOPT_URL` sites, and let a set-but-dummy key (or the URL
   override itself) enable AI. This is the *only* code change and it is
   provider-neutral — the cloud path stays the default and untouched.
2. First target: **LM Studio + Qwen3 8B (or 14B on ≥ 32 GB)** — full
   `tool_choice` compatibility means all three features exercise their real
   code paths.
3. Then empirically test **llama.cpp** (`llama-server --jinja`) as the
   lightweight headless option; verify the architect's forced `tool_choice`
   behaves (one latent-exit walk with stderr open shows clause a pass/fail
   immediately — the existing `failClause` diagnostics are the test harness).
4. Treat **Ollama** as supported-with-caveat: resolver + narration fine,
   architect works but relies on the model volunteering the call (`tool_choice`
   ignored), so expect more `You can't go that way.` fallbacks.
5. Watch the 8s timeout and the rejection rate in stderr; consider a slightly
   larger timeout via env override *only if* local latency proves to need it.

## Sources

- [Ollama — Anthropic compatibility docs](https://docs.ollama.com/api/anthropic-compatibility)
- [llama.cpp — Anthropic Messages API announcement (ggml-org, HF blog)](https://huggingface.co/blog/ggml-org/anthropic-messages-api-in-llamacpp)
- [llama.cpp PR #17570 — server: add Anthropic Messages API](https://github.com/ggml-org/llama.cpp/pull/17570)
- [LM Studio — Anthropic compatibility endpoints](https://lmstudio.ai/docs/developer/anthropic-compat) and [Messages](https://lmstudio.ai/docs/developer/anthropic-compat/messages)
- [LM Studio blog — use LM Studio models in Claude Code](https://lmstudio.ai/blog/claudecode)
- [Ollama issue #16922 — x-api-key auth on cloud endpoint](https://github.com/ollama/ollama/issues/16922)
- [llama-swap — model swapping for OpenAI/Anthropic-compatible servers](https://github.com/mostlygeek/llama-swap)
- [LiteLLM docs — Anthropic format + Ollama provider](https://docs.litellm.ai/docs/providers/ollama)
- [Connecting Claude Code to local LLMs (proxy vs native)](https://medium.com/@michael.hannecke/connecting-claude-code-to-local-llms-two-practical-approaches-faa07f474b0f)
- [SitePoint — best local LLM models 2026](https://www.sitepoint.com/best-local-llm-models-2026/)
- [LocalAIRun — best local LLM for coding 2026 (GLM, Qwen3.6, Gemma)](https://localairun.com/best-local-llm-for-coding/)
- [Micro Center — local LLMs by RAM tier (Apple Silicon unified memory)](https://www.microcenter.com/site/mc-news/article/best-local-llms-8gb-16gb-32gb-memory-guide.aspx)
