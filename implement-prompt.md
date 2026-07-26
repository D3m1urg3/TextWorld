# implement launch prompt (turn-latency-polish)

Paste the block below into a fresh session. Temporary — delete after use.

---

/implement .lore/work/plans/turn-latency-polish.md

The plan is **approved** and is the execution contract: work its 10 steps in order, one
step per commit, and do not re-open its decisions. The spec
`.lore/work/specs/turn-latency-polish.md` (REQ-LAT-1..14) is the source of truth for
requirements; the plan's per-step validation gates are how each is proven.

Scope is fixed. IN: env-gated profiling (`TEXTWORLD_PROFILE`, off by default), libcurl
connection reuse (persistent handle, reset-not-cleanup, global init/cleanup once,
`CURLOPT_NOSIGNAL`), per-role model (resolve → `claude-haiku-4-5`; narrate/generate stay
`claude-opus-4-8`; `TEXTWORLD_MODEL` still overrides all roles). OUT (do not build, do not
"while I'm here" it): background pregen, SSE streaming, prompt caching, fast mode, HTTP/2,
curl multi. Keep the forward-compat hooks the plan records in `aihttp.hpp`
(global init before threads, NOSIGNAL, one-handle-per-thread, never share the connection
cache).

Already settled — implement as written, don't re-litigate:
- One **shared** persistent easy handle for all three roles, in a new `src/aihttp.{hpp,cpp}`.
  This deliberately reverses the old "deliberately REIMPLEMENTS" triplication decision;
  rewrite those three comment blocks (`prose.cpp`, `nlresolve.cpp`, `architect.cpp`) to say so.
- `HttpResponse` / `HttpTransport` (`src/prose.hpp:48-53`) **do not change**. All new
  telemetry is emitted from inside the production transport. Step 8's gate is *zero test
  changes* — if you find yourself editing fake-transport tests, you broke the seam.
- `TEXTWORLD_PROFILE=0` counts as off; a test-only `profileRefreshEnabled()` exists so the
  gate can be flipped in-process.
- `generate` nests inside `tick` (the architect call happens in `systems.cpp:97-101`, inside
  the transaction) and is tagged `nested_in=tick`.
- `CURLOPT_TCP_KEEPALIVE, 1L` is in.

How to work (from memory):
- Do this **inline**. No implement subagents — they are a token sink here.
- Steps 1–8 are deterministic and offline. Pass each step's gate before moving on:
  `cmake --build build` clean and `./build/tests` green, plus that step's named test /
  inspection grep. Don't batch steps.
- **Stop before Step 9 and ask.** It is the one live-LLM step: a fixed ~8-line script run
  across four short configs, mechanical observations only, no re-runs to chase nicer
  numbers. Get my go-ahead and confirm the token/spend budget first.
- Step 10 is the spec sweep; when it passes, move the spec's status to `implemented`.

Build: `cmake --build build`; tests: `./build/tests` (offline; live paths gated behind
`TEXTWORLD_AI_LIVE_TEST=1`).
