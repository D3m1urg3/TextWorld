// Turn profiling: permanent, gated instrumentation for where a turn's time
// goes (REQ-LAT-1..-6). Off by default — with TEXTWORLD_PROFILE unset the only
// cost on the turn path is one monotonic clock read per ScopedStage plus a
// cached bool test, and nothing is ever written.
//
// Two record kinds, one machine-parseable `key=value` line each (REQ-LAT-5),
// written to a sink that defaults to **stderr** so player output on stdout is
// never interleaved:
//   twprof kind=stage turn=7 stage=narrate ms=1843.221
//   twprof kind=call  turn=7 role=narrate model=... status=200 ... input_tokens=…
//
// Absent, never zero-faked (REQ-LAT-2): a stage that does not run on a turn
// constructs no ScopedStage, so it emits no record. `generate` is the one
// NESTED stage — it runs inside the architect call that `tick` makes
// (systems.cpp resolveGo, inside the tick transaction), so its record carries
// `nested_in=tick` and an aggregator must not add it to a turn total.
//
// PRIVACY: no record may ever contain an API key, a prompt, a response body,
// or player text. Only stage names, role names, a model id, timings, counts.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

// TEXTWORLD_PROFILE set, non-empty, and not exactly "0" (the "0 means off"
// convention aiNarrationEnabled() already uses for TEXTWORLD_AI). The getenv
// happens ONCE per process — the result is cached in a file-static bool, so
// this is a plain bool read on the turn path (REQ-LAT-1).
bool profilingEnabled();

// TEST-ONLY. Re-reads TEXTWORLD_PROFILE into the cache so a test can flip the
// gate inside one process. Production code never calls this.
void profileRefreshEnabled();

// One semantic stage of a turn. `stage` and `nestedIn` are string literals
// owned by the caller (never freed here); `nestedIn` is nullptr for a
// top-level stage.
struct StageRecord {
    const char* stage = "";
    int64_t turn = 0;
    double ms = 0.0;
    const char* nestedIn = nullptr;
};

// One network call made inside a stage. `failed` covers both a transport
// error and a non-200; a failed call NEVER carries token counts (REQ-LAT-4 —
// note the failure, do not fabricate). `tokensKnown` false on a 200 whose body
// had no readable `usage` emits `tokens=unknown`, so an aggregator can tell
// "not reported" from "not parsed".
struct CallRecord {
    const char* role = "";
    std::string model;
    int64_t turn = 0;
    bool failed = false;
    long status = 0;
    int64_t namelookupUs = 0;
    int64_t connectUs = 0;
    int64_t appconnectUs = 0;
    int64_t starttransferUs = 0;
    int64_t totalUs = 0;
    long long inputTokens = 0;
    long long outputTokens = 0;
    bool tokensKnown = false;
};

// Pure formatters — one line, no trailing newline. Pure so the record FORMAT
// (REQ-LAT-5) is testable without capturing a stream.
std::string formatStage(const StageRecord& record);
std::string formatCall(const CallRecord& record);

// Format + write one line to the sink. No-op unless profilingEnabled().
void profileEmit(const StageRecord& record);
void profileEmit(const CallRecord& record);

// Redirect records. The default sink writes the line + '\n' to stderr; an
// empty function restores that default. Used by tests to capture records.
void profileSetSink(std::function<void(const std::string&)> sink);

// Process-local turn sequence, so every record of one turn shares a `turn=`
// value. Independent of meta.turn — no extra SELECT on the turn path.
int64_t profileNextTurn();
int64_t profileCurrentTurn();

// RAII stage timer. The constructor reads a monotonic clock UNCONDITIONALLY
// (that read is the whole REQ-LAT-1 overhead budget); the destructor computes
// the delta and emits. Non-copyable, non-movable: one scope, one record.
class ScopedStage {
  public:
    explicit ScopedStage(const char* stage, const char* nestedIn = nullptr)
        : stage_(stage),
          nestedIn_(nestedIn),
          start_(std::chrono::steady_clock::now()) {}
    ~ScopedStage();

    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;

  private:
    const char* stage_;
    const char* nestedIn_;
    std::chrono::steady_clock::time_point start_;
};
