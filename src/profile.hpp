// Turn profiling: permanent, gated instrumentation for where a turn's time
// goes (REQ-LAT-1..-6). Off by default — below TEXTWORLD_LOG_LEVEL=debug the
// only cost on the turn path is one monotonic clock read per ScopedStage plus a
// cached enum test, and nothing is ever formatted or written (REQ-LOG-25).
//
// Record kinds, one machine-parseable `key=value` line each (REQ-LAT-5),
// emitted as DEBUG **log entries** (REQ-LOG-22): the payload below is carried
// byte-for-byte after the logger's standard six-field prefix (REQ-LOG-23), and
// lands in the session log rather than on the terminal (REQ-LOG-1):
//   twprof kind=stage turn=7 stage=narrate ms=1843.221
//   twprof kind=call  turn=7 role=narrate model=... status=200 ... input_tokens=…
//   twprof kind=dwell turn=7 ms=1234.567
//
// THREAD SAFETY (REQ-PREGEN-22, widened by REQ-LOG-15). Records are emitted
// from the main thread and both workers. Serialization is log.cpp's now — one
// entry is one line written under one mutex, never interleaved, never
// truncated. Nothing here needs to restate it, but the invariant "one record is
// one write" still governs any new sink or record kind.
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
#include <optional>
#include <string>

// True at TEXTWORLD_LOG_LEVEL=debug and nowhere else (REQ-LOG-22 — the
// separate profiling variable is retired, and there is now ONE switch for the
// whole engine). The getenv happens ONCE per process and the threshold is
// cached, so this is a plain enum comparison on the turn path (REQ-LAT-1,
// REQ-LOG-25).
bool profilingEnabled();

// TEST-ONLY. Re-reads TEXTWORLD_LOG_LEVEL into the cache so a test can flip the
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
//
// `background` (REQ-PREGEN-24) marks a call made by the pre-generation worker
// rather than on a turn's critical path. It formats as ` background=1` and is
// OMITTED ENTIRELY when false, so every foreground record — every record that
// existed before pre-generation — is byte-identical to what it was.
struct CallRecord {
    const char* role = "";
    std::string model;
    int64_t turn = 0;
    bool failed = false;
    long status = 0;
    bool background = false;
    int64_t namelookupUs = 0;
    int64_t connectUs = 0;
    int64_t appconnectUs = 0;
    int64_t starttransferUs = 0;
    int64_t totalUs = 0;
    long long inputTokens = 0;
    long long outputTokens = 0;
    bool tokensKnown = false;
};

// How long the PLAYER took, not the engine: the gap between a turn's output
// being flushed and the next input line coming back (REQ-PREGEN-25). This is
// the number that says whether one serial pre-generation worker can keep up
// with a reader, and it is the only record here that measures a human.
// `turn` is the turn just COMPLETED — the record is emitted after that turn's
// output and before the next profileNextTurn().
struct DwellRecord {
    int64_t turn = 0;
    double ms = 0.0;
};

// The outcome of ONE latent-exit walk (REQ-PREGEN-23) — the record that makes
// pre-generation's hit rate measurable rather than assumed. `outcome` is one of
// the four canonical names, matching PregenOutcome:
//
//   twprof kind=pregen turn=7 outcome=hit age_turns=3
//   twprof kind=pregen turn=7 outcome=waited wait_ms=2140.118
//   twprof kind=pregen turn=7 outcome=ran_queued run_ms=6912.004
//   twprof kind=pregen turn=7 outcome=miss
//
// Each optional key belongs to exactly ONE outcome and appears nowhere else; a
// miss carries none. `ageTurns` is how stale the committed candidate was, in
// world turns (REQ-PREGEN-13) — reported only, it invalidates nothing.
//
// Note what is NOT here: a `generate` stage. That stage keeps its one meaning,
// "the synchronous architectGenerate ran", so it accompanies a MISS alone. A
// ran_queued path can only cover the architect's Phase 1 — Phase 2 happens
// later, in the commit — so reusing the label would give one stage name two
// different spans and quietly skew any miss-versus-ran_queued comparison. The
// run and wait costs ride on this record instead.
struct PregenRecord {
    int64_t turn = 0;
    const char* outcome = "";  // string literal owned by the caller
    std::optional<int64_t> ageTurns;  // hit only
    std::optional<double> waitMs;     // waited only
    std::optional<double> runMs;      // ran_queued only
};

// Pure formatters — one line, no trailing newline. Pure so the record FORMAT
// (REQ-LAT-5) is testable without capturing a stream.
std::string formatStage(const StageRecord& record);
std::string formatCall(const CallRecord& record);
std::string formatDwell(const DwellRecord& record);
std::string formatPregen(const PregenRecord& record);

// Format + write one line to the sink. No-op unless profilingEnabled().
// Serialized across threads (REQ-PREGEN-22): one record, one atomic line.
void profileEmit(const StageRecord& record);
void profileEmit(const CallRecord& record);
void profileEmit(const DwellRecord& record);
void profileEmit(const PregenRecord& record);

// Redirect records — the LOGGER's sink, under profiling's old name so no call
// site had to move. A sink receives the whole formatted entry: six-field prefix
// AND twprof payload (REQ-LOG-27). An empty function restores the default
// writer. Used by tests to capture records.
void profileSetSink(std::function<void(const std::string&)> sink);

// Process-local turn sequence, so every record of one turn shares a `turn=`
// value. Independent of meta.turn — no extra SELECT on the turn path.
//
// The counter itself lives in log.cpp (REQ-LOG-13 makes it a logging concern,
// and this unit depends on the logger); these two are forwarders. It is a
// std::atomic, so profileCurrentTurn() is safe to read from ANY thread while
// the main thread advances it (REQ-PREGEN-22). Only the main thread ever calls
// profileNextTurn().
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

// RAII dwell timer, ScopedStage's shape applied to the player's own thinking
// time (REQ-PREGEN-25). Wrap the read of the next input line in one: the
// constructor takes the clock, the destructor emits a DwellRecord stamped with
// profileCurrentTurn() — the turn just completed, since the next turn has not
// been numbered yet. Non-copyable, non-movable: one scope, one record.
class ScopedDwell {
  public:
    ScopedDwell() : start_(std::chrono::steady_clock::now()) {}
    ~ScopedDwell();

    ScopedDwell(const ScopedDwell&) = delete;
    ScopedDwell& operator=(const ScopedDwell&) = delete;

  private:
    std::chrono::steady_clock::time_point start_;
};
