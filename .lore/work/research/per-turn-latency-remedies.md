---
title: "Per-turn latency remedies: libcurl reuse, Claude API levers, pregen concurrency"
date: 2026-07-26
status: active
tags: [performance, latency, libcurl, connection-reuse, claude-api, streaming, prompt-caching, concurrency, threading, profiling]
modules: [nlresolve, prose, architect, loop]
related: [.lore/work/brainstorm/performance-polish-action-latency.md]
---

# Per-turn latency remedies: libcurl reuse, Claude API levers, pregen concurrency

Research to replace the estimates in the [performance brainstorm](../brainstorm/performance-polish-action-latency.md)
with concrete, current facts before implementing. Covers the three remedies
(connection reuse, model/API levers, background pregen) plus the curl timing
fields for the profiling step.

## Key Findings

1. **Connection reuse is "the primary key to high performance" and it's automatic —
   but only if you keep the handle alive.** Our code throws it away every call
   (`curl_easy_init()` → `curl_easy_cleanup()` per request in `nlresolve.cpp`,
   `prose.cpp`, `architect.cpp`), so every call pays a fresh DNS + TCP + TLS
   handshake. A persistent handle reuses the connection, the TLS session, and the
   DNS cache. Documented real-world gain: repeated same-host transfers dropped from
   130–260 ms to **30–40 ms** just by reusing the handle. Our two-to-three
   sequential calls per turn are the ideal case for this.

2. **The fix is small and low-risk:** stop calling `curl_easy_cleanup()` per
   request. Keep one long-lived easy handle, `curl_easy_reset()` it between calls
   (this preserves the connection pool and DNS cache; it only clears options), re-set
   the per-call options, and reuse. Call `curl_global_init()` once at startup.

3. **Thread-safety rule for pregen is simple: one easy handle per thread, never
   shared.** "You must never use a single handle from more than one thread at any
   given time." Handles can be *passed* between threads, just not used concurrently.
   Sharing the connection cache across concurrent threads via the share interface is
   **explicitly unsafe (known bug)** — do not try it. So: main thread keeps its
   handle, the pregen worker gets its *own* handle. Each still gets per-handle reuse.

4. **Claude API model tiering is real and large.** Haiku 4.5 is $1/$5 per 1M
   (in/out) vs Opus 4.8 $5/$25 — **5× cheaper**, and Haiku is the documented tier for
   "simple, speed-critical tasks." The resolve step (lower raw text → a fixed verb) is
   exactly that. Narrate/generate (prose authoring) stay on a stronger model.

5. **Prompt caching won't help our prompts — they're too small.** Opus 4.8's minimum
   cacheable prefix is **4,096 tokens**; Haiku 4.5 is also 4,096; Sonnet 4.6 / Fable 5
   are 2,048. Our fixed prefixes (~1,100–1,600 tokens) are below all of these, so
   `cache_control` silently no-ops (`cache_creation_input_tokens: 0`). Don't bother
   with caching until/unless prompts grow past the threshold.

6. **Streaming (SSE) is the lever for *perceived* latency**, separate from wall-clock.
   It doesn't make the model finish faster, but first tokens arrive far sooner, and it
   lets us print prose as it generates instead of a dead prompt. Relevant to narrate;
   not to resolve (resolve returns a tool call we act on, nothing to show).

7. **Fast mode exists but is a poor fit here.** Opus 4.8/4.7 only, ~2.5× output
   tokens/sec, **premium pricing**, beta, its own rate limit. It speeds *output
   generation*, not time-to-first-byte, and costs more. Reach for streaming + Haiku
   first; fast mode is a later, narrow option for prose if output length dominates.

8. **curl already measures the setup-vs-thinking split** via `curl_easy_getinfo` —
   no external profiler. Use the `_T` (microsecond) variants. Timeline:
   `NAMELOOKUP → CONNECT → APPCONNECT(TLS) → PRETRANSFER → STARTTRANSFER(TTFB) → TOTAL`.

## 1. libcurl connection reuse

**Current state (the bug):** each of the three transports does
`curl_easy_init(); … curl_easy_setopt …; curl_easy_perform(); curl_easy_cleanup();`.
`cleanup()` destroys the handle and *with it the connection pool, TLS session cache,
and DNS cache*. So every single call reconnects from scratch — the most expensive
possible pattern.

**How reuse works (from curl docs):**
- libcurl *automatically and always* tries to reuse connections unless told not to
  (`CURLOPT_FORBID_REUSE`). On the **easy** API the connection pool is owned by the
  **easy handle** — so reusing the same handle is what enables reuse.
- Reuse skips: DNS resolution (cached), TCP connect, and the TLS handshake (TLS
  session reuse). These are exactly the `NAMELOOKUP`/`CONNECT`/`APPCONNECT` buckets.

**Recommended pattern for us (single-threaded main loop):**
- `curl_global_init(CURL_GLOBAL_DEFAULT)` once at program start;
  `curl_global_cleanup()` at exit.
- Create **one** persistent easy handle (or one per role if the per-call option sets
  differ enough that reset churn isn't worth it — but a single shared handle to
  `api.anthropic.com` is fine since all three hit the same host).
- Between calls: `curl_easy_reset(handle)` (clears options, **keeps** the connection
  pool + DNS cache), then re-apply the per-call options (URL, POST body, headers,
  write callback, timeout), then `curl_easy_perform()`. Do **not** `cleanup()` until
  shutdown.
- Set `CURLOPT_NOSIGNAL, 1L` (required once we add threads; harmless now).
- Optional: `CURLOPT_TCP_KEEPALIVE, 1L` to keep idle connections warm across turns
  (a player thinks between turns; the connection can otherwise be closed by the
  server or a NAT idle timeout). Consider `CURLOPT_MAXCONNECTS` if we end up with
  multiple handles.

**HTTP/2:** the Anthropic API supports HTTP/2. With a reused handle libcurl can
negotiate it (`CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS`). Multiplexing only
matters for *concurrent* requests on one connection — our per-turn calls are
sequential, so HTTP/2's main benefit here is just being the modern default; keep-alive
+ TLS reuse is where the win is. Not a priority; note and move on.

**Multi interface:** owns the connection pool at the *multi* handle level, letting you
recreate easy handles freely without losing the pool, and reuse connections across
different easy handles. Overkill for us now — a single persistent easy handle is
simpler and gets the same reuse. Revisit only if we ever want true concurrent
in-flight requests (e.g. parallelizing pregen of multiple neighbors).

## 2. Claude API latency & cost levers

Authoritative pricing/limits from the `claude-api` skill (cached 2026-06-24):

| Model | In $/1M | Out $/1M | Cache min prefix | Notes |
|---|---|---|---|---|
| Haiku 4.5 (`claude-haiku-4-5`) | $1 | $5 | 4,096 tok | "simple, speed-critical tasks" |
| Sonnet 5 (`claude-sonnet-5`) | $3 ($2 intro) | $15 ($10 intro) | 2,048 tok | |
| Opus 4.8 (`claude-opus-4-8`) | $5 | $25 | 4,096 tok | current default in code |

- **Model tiering:** move the **resolve** call to Haiku (5× cheaper, faster). It's a
  constrained tool-call classification — Haiku's sweet spot. Keep narrate (and
  generate) on the stronger model where prose quality matters; re-evaluate generate on
  Sonnet 5 later if cost/latency wants it. Override is already wired: `TEXTWORLD_MODEL`
  env var, but that's global — we'd want a **per-role** model choice, so this needs a
  small code change (resolve hardcodes/params Haiku, narrate/generate keep the current
  default). Model IDs are complete as-is; never append date suffixes.
- **Prompt caching — skip it.** Cache economics only matter above the minimum prefix
  (4,096 tok on Opus/Haiku, 2,048 on Sonnet). Our prompts are ~1–2K tokens; a
  `cache_control` marker silently won't cache (verify via
  `usage.cache_creation_input_tokens == 0`). Revisit only if prompts grow.
- **Streaming (SSE):** cuts perceived latency by delivering first tokens fast; enables
  printing narration as it streams instead of blocking. Raw SSE is a sequence of
  `content_block_delta` events with `delta.text`. Applies to **narrate** only.
  Trade-off: adds parsing complexity (we currently buffer the whole body via the write
  callback). Worth it for the perceived-latency thread flagged in the brainstorm.
- **Fast mode:** Opus 4.8/4.7 only, ~2.5× output speed, premium price, beta, separate
  rate limit. Speeds output generation, not TTFB. Deprioritize; streaming + Haiku are
  the better first moves.

## 3. Concurrency for background pregen

The brainstorm's design already sidesteps the hardest part: **background does only the
network call; the SQLite write stays on the main thread inside a tick.** Research
confirms this is the right call and clarifies the threading rules.

**libcurl side:**
- **One handle per thread, never shared concurrently.** Main thread keeps its handle;
  the pregen worker gets its own. Each still reuses its own connection/TLS/DNS.
- **Do NOT** share the connection cache across the two threads via the share interface
  — sharing *connections* across concurrent threads is a known-unsafe bug. (The share
  interface is safe for DNS/SSL-session/cookie data *with* your own lock funcs, but
  connections specifically are not — and we don't need it anyway.)
- `curl_global_init()` **must** be called before any thread starts using libcurl (it's
  not thread-safe to lazy-init from multiple threads). Call it once at startup.
- Set `CURLOPT_NOSIGNAL, 1L` on all handles in a multithreaded program.

**SQLite side:**
- No new SQLite threading needed *if we hold the line*: all DB access stays on the main
  thread. The background thread never touches `Db`. A candidate room is pure in-memory
  data (prose + declared exits) parked for the main thread; the main thread commits it
  inside a normal tick on cache hit.
- (For the record: SQLite's default build is "serialized" and a single connection must
  not be used from two threads at once anyway. Our design avoids the question entirely
  by keeping one connection on one thread.)

**Parking results for the main thread:** a small thread-safe handoff — a
mutex-guarded map keyed by the latent-exit identity, or a lock-free single-slot per
in-flight prefetch. The main thread checks the map at the start of a movement tick;
hit → consume + commit, miss → synchronous fallback (today's path). Keep the handoff
tiny and the worker's only job the curl call + JSON parse.

## 4. curl timing fields for the profiling step

`curl_easy_getinfo(handle, CURLINFO_*_TIME_T, &microseconds)` after each
`curl_easy_perform()`. Use the `_T` (microsecond `curl_off_t`) variants for precision.
Hierarchy (each is cumulative from transfer start):

| Field | Marks | Bucket |
|---|---|---|
| `CURLINFO_NAMELOOKUP_TIME_T` | DNS resolution done | setup |
| `CURLINFO_CONNECT_TIME_T` | TCP connect done | setup |
| `CURLINFO_APPCONNECT_TIME_T` | TLS handshake done | setup |
| `CURLINFO_PRETRANSFER_TIME_T` | ready to send request | setup |
| `CURLINFO_STARTTRANSFER_TIME_T` | **first byte received (TTFB)** | model thinking |
| `CURLINFO_TOTAL_TIME_T` | transfer complete | total |

**Reading the split:** on a *cold* connection, `APPCONNECT − NAMELOOKUP` is the
handshake cost (what connection reuse eliminates). On a *reused* connection those
early stages are ~0 and `STARTTRANSFER` dominates (pure model TTFB). So the profiling
run will directly show, per call, how much reuse can save vs. how much is irreducible
model latency. Also grab `usage.input_tokens` / `output_tokens` from each response body
for the per-role token counts the brainstorm asked for.

## Implications for TextWorld (concrete)

1. **Profiling instrumentation:** per-phase monotonic clock in `runTurn` + the six
   `CURLINFO_*_TIME_T` fields per call + per-role token counts from the response
   JSON. Scripted ~20-turn run + AI-off baseline. This is the immediate next build.
2. **Connection reuse:** replace per-call `init/cleanup` with a persistent handle +
   `curl_easy_reset()`; add `curl_global_init` at startup, `CURLOPT_NOSIGNAL`,
   optionally `CURLOPT_TCP_KEEPALIVE`. Small, mechanical, helps every turn.
3. **Per-role model:** resolve → Haiku; narrate/generate keep the current default.
   Needs a per-role model selection (the existing `TEXTWORLD_MODEL` is global).
4. **Pregen:** worker thread with its *own* curl handle, network-only, parks in-memory
   candidates; main thread commits on hit inside a tick, synchronous fallback on miss.
   `curl_global_init` before spawning; `CURLOPT_NOSIGNAL` on both handles.
5. **Perceived latency (optional thread):** SSE streaming for narrate to kill the
   dead-prompt feel. More complex (incremental parse); decide after profiling shows
   how much wall-clock is TTFB vs. output generation.
6. **Skip for now:** prompt caching (prompts too small), fast mode (premium, wrong
   axis), HTTP/2 tuning and the multi interface (no concurrent in-flight requests yet).

## Sources

- [everything.curl.dev — Performance](https://everything.curl.dev/libcurl/performance.html)
- [everything.curl.dev — Connection reuse](https://everything.curl.dev/transfers/conn/reuse.html)
- [curl.se — libcurl thread safety (threadsafe.html)](https://curl.se/libcurl/c/threadsafe.html)
- [curl.se — CURLOPT_FORBID_REUSE](https://curl.se/libcurl/c/CURLOPT_FORBID_REUSE.html)
- [curl.se — curl_easy_getinfo](https://curl.se/libcurl/c/curl_easy_getinfo.html)
- [curl.se — CURLINFO_TOTAL_TIME](https://curl.se/libcurl/c/CURLINFO_TOTAL_TIME.html)
- [Schneide Blog — Keeping connections alive with libcurl](https://schneide.blog/2017/10/02/keeping-connections-alive-with-libcurl/)
- Claude API pricing / cache minimums / streaming / fast mode: `claude-api` skill reference (cached 2026-06-24)
