// Shared AI HTTP layer: the one place the three AI roles agree on which model
// they call and (from Step 6) how they call it.
//
// This file deliberately REVERSES the earlier "each unit reimplements its own
// curlTransport" stance. That decision shared only the HttpTransport *type* so
// tests could inject fakes — a goal this file leaves fully intact, since the
// seam type and signature do not change. What triplication cannot survive is a
// PERSISTENT handle: handle ownership, curl_easy_reset + full option
// re-application, getinfo timing capture, and profile emission would be
// written three times, i.e. three chances to leak an option.
#pragma once

#include <atomic>
#include <string>

#include "prose.hpp"  // HttpResponse / HttpTransport — the seam, UNCHANGED

// The four AI call sites. The names are exactly the strings the profile
// records carry in their `role=` field. Bard is its own role rather than a
// reuse of Generate precisely so a profile log can tell a bard wake from a
// pregen room job — the two workloads with the most different shapes.
enum class AiRole { Resolve, Narrate, Generate, Bard };

const char* roleName(AiRole role);

// The uniform per-call transport budget, in seconds. Named so that the ONE
// deliberate exception to it — kBardOvertureTimeoutSeconds, in bard.hpp — has
// something to be an exception TO. Every other call site takes this default.
//
// 20 rather than 8, sized off the ARCHITECT — the heaviest call the binary
// makes (~3.1k input tokens, a required create_room tool call, max_tokens 1024,
// and not streamed, so nothing returns until the whole room is generated).
// Measured over 24 live calls on claude-opus-4-8: p50 6.9 s, p95 9.8 s,
// max 14.6 s — 42% of them exceeded the old 8 s budget. A timeout there is not
// cosmetic: curl discards the response and the player gets "You can't go that
// way." on an exit the Exits: line just advertised as walkable.
//
// Raising this never slows a call that already succeeds — a timeout only ever
// bites the tail. Resolve (0.8-1.9 s) and narrate (2.0-3.3 s) never approach
// either value; what changes for them is only how long a stalled call waits
// before falling back to the deterministic parser or the template renderer.
inline constexpr long kAiHttpTimeoutSeconds = 20;

// The model id for one role. THE single place the precedence rule lives —
// exactly two levels (REQ-LAT-13):
//
//   1. TEXTWORLD_MODEL, when set AND non-empty, applies to ALL roles;
//   2. otherwise the per-role default (REQ-LAT-12):
//        Resolve  -> claude-haiku-4-5   (fast/cheap; the gate still governs)
//        Narrate  -> claude-opus-4-8    (prose quality)
//        Generate -> claude-opus-4-8    (prose quality)
//        Bard     -> claude-opus-4-8    (authoring quality)
//
// Per-role environment overrides (TEXTWORLD_MODEL_RESOLVE and friends) are
// OUT OF SCOPE by decision, not by oversight: their absence is deliberate. If
// they are ever added they layer on top of level 2, below TEXTWORLD_MODEL.
std::string modelForRole(AiRole role);

// Token counts read back from an Anthropic response body's `usage` object.
// `known` false means the counts could not be read — NEVER that they were
// zero (REQ-LAT-4: note the failure, do not fabricate).
struct AiUsage {
    bool known = false;
    long long inputTokens = 0;
    long long outputTokens = 0;
};

// Pure. Reads usage.input_tokens / usage.output_tokens out of a response body.
// NEVER throws: a non-JSON body, a missing `usage`, or non-integer fields all
// yield known = false.
AiUsage parseUsage(const std::string& responseBody);

// Pure. Reads "model" back out of a REQUEST body, so a profile record can name
// the model actually sent without widening the HttpTransport seam to carry it.
// Returns "" on any parse failure. Only ever called when profiling is on.
std::string modelFromRequestBody(const std::string& requestBody);

// --- the shared persistent-handle HTTP client -------------------------------
//
// THREADING CONTRACT (REQ-LAT-11, REQ-PREGEN-8, REQ-BARD-WAKE-17), written
// down for the background pre-generation work and now CASHED by two workers:
//   * The shared handle behind anthropicPost() is MAIN-THREAD ONLY.
//   * One easy handle per thread. NEVER share a handle between threads, and
//     never share a connection cache across threads (that is what a curl share
//     handle would be for — deliberately not used).
//   * AiHttpWorkerClient below is THE TYPE EVERY SANCTIONED WORKER HANDLE
//     USES. There are now TWO live instances of it in the binary, not one:
//
//        the pre-generation worker's, owned by pregen.cpp's thread
//        the bard's wake worker's,   owned by bardworker.cpp's thread
//
//     Each is constructed on, used by, and destroyed on ITS OWN thread, and
//     neither ever names the shared main-thread handle. Adding a third worker
//     means adding a third instance of this type and a third guard — never a
//     second thread sharing an existing handle.
//   * aiHttpInit() must run BEFORE any thread that touches libcurl is created;
//     curl_global_init is not thread-safe and must not race lazy init.
//     Symmetrically, every AiHttpWorkerClient must be DESTROYED — and the
//     thread owning it joined — BEFORE aiHttpShutdown() (REQ-PREGEN-19,
//     REQ-BARD-WAKE-18). In main() that ordering is structural, not
//     remembered: PregenGuard (pregen.hpp) and BardGuard (bardworker.hpp) are
//     both declared BELOW AiHttpGuard, so reverse destruction joins both
//     threads before curl_global_cleanup on every exit path. "Below
//     AiHttpGuard" is the load-bearing invariant; their order relative to each
//     other is not, since the two workers are independent.
//   * CURLOPT_NOSIGNAL is already set on every call, so a worker thread
//     cannot be killed by libcurl's alarm-based DNS timeout.

// curl_global_init(CURL_GLOBAL_DEFAULT), once. Idempotent (REQ-LAT-7).
void aiHttpInit();

// Destroys the persistent handle FIRST, then calls curl_global_cleanup(). That
// ordering is the whole point: a handle outliving global cleanup is undefined
// behavior. Safe when no handle was ever created — the common case, since every
// offline test run and every AI-off session makes no call at all — and
// idempotent, because the guard's destructor also runs on error exits.
void aiHttpShutdown();

// The ONE production transport, behavior-identical to the three per-unit
// curlTransports it replaces (REQ-LAT-10): same URL, same three headers with
// ANTHROPIC_API_KEY read at CALL time into x-api-key only, the same total
// timeout by default, transportError on any curl failure, response code read
// only on CURLE_OK, and NO retries.
//
// `timeoutSeconds` is a PARAMETER rather than a constant only so the bard's
// overture can name its one sanctioned exception (REQ-BARD-WAKE-5). Every
// existing caller takes the default and is unchanged.
//
// What differs: the easy handle PERSISTS across calls (REQ-LAT-8), so the
// connection, TLS session, and DNS cache survive between calls and between
// turns. Each call begins with curl_easy_reset and then re-applies EVERY
// per-call option, so nothing leaks from one call into the next.
//
// When profiling is on it also emits one call record: curl's five microsecond
// timing fields, the role, the model actually sent, the status, and the token
// counts from the response usage.
//
// SCOPE BOUNDARY for "or fell back" (REQ-LAT-4): this transport reports what
// the API did. It cannot see a DOWNSTREAM rejection — a 200 that the prose,
// resolver, or architect validation gate then refuses — and must not try to.
// Real tokens were spent on such a call, so the record reports them honestly;
// the fallback itself is already announced on the same stderr stream by each
// unit's one-line clause diagnostic. Do not "fix" this by plumbing gate
// results back through the seam.
HttpResponse anthropicPost(const std::string& requestBody, AiRole role,
                           long timeoutSeconds = kAiHttpTimeoutSeconds);

// Bind a role (and, optionally, a non-default budget) into the existing seam.
// HttpTransport's signature is unchanged, so every fake-transport test keeps
// working exactly as before.
HttpTransport makeAnthropicTransport(AiRole role,
                                     long timeoutSeconds = kAiHttpTimeoutSeconds);

// ONE easy handle, owned by the thread that constructs it (REQ-PREGEN-8) — the
// type every sanctioned worker handle uses, per the threading contract above.
//
// NEVER touches the shared main-thread handle; no curl share handle exists, so
// the connection cache is deliberately NOT shared across the two threads. That
// costs the worker a fresh connect per call and buys the absence of a whole
// class of race; the worker is off the critical path, so the trade is free.
//
// Construct and destroy it on the SAME thread, and only between aiHttpInit()
// and aiHttpShutdown(). `abort` is a flag the OWNER of this object may set from
// another thread to tear down an in-flight transfer promptly (REQ-PREGEN-20);
// it is polled by a libcurl progress callback, so its granularity is libcurl's
// callback cadence (about a second while idle-waiting on TTFB), not
// instantaneous. Pass nullptr for no abort.
//
// post() is otherwise behavior-identical to anthropicPost: same URL, headers,
// 8 s timeout, one attempt, no retries. Its profile records carry
// background=1 (REQ-PREGEN-24).
class AiHttpWorkerClient {
  public:
    explicit AiHttpWorkerClient(const std::atomic<bool>* abort);
    ~AiHttpWorkerClient();

    AiHttpWorkerClient(const AiHttpWorkerClient&) = delete;
    AiHttpWorkerClient& operator=(const AiHttpWorkerClient&) = delete;

    HttpResponse post(const std::string& requestBody, AiRole role);

  private:
    void* handle_ = nullptr;  // CURL*, opaque here so curl.h stays out of this
                              // header (every AI unit includes it)
    const std::atomic<bool>* abort_ = nullptr;
};

// Process-lifetime RAII for the two calls above (REQ-LAT-7). Instantiate ONE
// of these as the first local in main(), so its destructor covers every exit
// path the binary has — normal return, quit, EOF, and each catch.
//
// Explicit RAII, deliberately NOT a function-local static: a static's
// destructor runs AFTER main returns, i.e. after anything main itself cleaned
// up, which is precisely the ordering bug this type exists to avoid.
struct AiHttpGuard {
    AiHttpGuard() { aiHttpInit(); }
    ~AiHttpGuard() { aiHttpShutdown(); }

    AiHttpGuard(const AiHttpGuard&) = delete;
    AiHttpGuard& operator=(const AiHttpGuard&) = delete;
};
