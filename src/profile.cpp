// Profiling mechanism (see profile.hpp for the contract). No call sites live
// here — loop.cpp, architect.cpp and aihttp.cpp own those.
#include "profile.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace {

// TEXTWORLD_PROFILE: set, non-empty, and not exactly "0".
bool readProfileEnv() {
    const char* v = std::getenv("TEXTWORLD_PROFILE");
    if (v == nullptr || v[0] == '\0') return false;
    return std::string(v) != "0";
}

// Cached at static-init time: the getenv happens once per process (REQ-LAT-1).
// No other translation unit reads this during ITS static init, so the
// initialization order is unobservable.
bool g_enabled = readProfileEnv();

// Process-local turn sequence (not meta.turn). ATOMIC because the pregen
// worker reads it via profileCurrentTurn() to stamp its background call
// records while the main thread advances it between turns (REQ-PREGEN-22).
// Only the main thread ever writes.
std::atomic<int64_t> g_turn{0};

// Empty => the default stderr sink.
std::function<void(const std::string&)>& sink() {
    static std::function<void(const std::string&)> s;
    return s;
}

// Serializes the whole of write() — the sink lookup, the sink call, and the
// default fprintf alike (REQ-PREGEN-22). Records come from two threads, and
// this is what makes one record one ATOMIC line rather than two threads'
// bytes braided together. Note the default sink is a SINGLE fprintf, not a
// write-then-newline pair: splitting it would defeat the point of the lock.
std::mutex& sinkMutex() {
    static std::mutex m;
    return m;
}

void write(const std::string& line) {
    const std::lock_guard<std::mutex> lock(sinkMutex());
    if (sink()) {
        sink()(line);
        return;
    }
    std::fprintf(stderr, "%s\n", line.c_str());
}

// Milliseconds with three decimals — microsecond resolution, no exponent.
std::string formatMs(double ms) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", ms);
    return buf;
}

}  // namespace

bool profilingEnabled() { return g_enabled; }

void profileRefreshEnabled() { g_enabled = readProfileEnv(); }

int64_t profileNextTurn() { return ++g_turn; }

int64_t profileCurrentTurn() { return g_turn; }

void profileSetSink(std::function<void(const std::string&)> s) {
    sink() = std::move(s);
}

std::string formatStage(const StageRecord& record) {
    std::string out = "twprof kind=stage turn=" + std::to_string(record.turn) +
                      " stage=" + record.stage + " ms=" + formatMs(record.ms);
    // Trailing so the top-level form above is a literal prefix of the nested
    // one (greppable); key=value parsing is order-independent anyway.
    if (record.nestedIn != nullptr) {
        out += " nested_in=";
        out += record.nestedIn;
    }
    return out;
}

std::string formatCall(const CallRecord& record) {
    std::string out = "twprof kind=call turn=" + std::to_string(record.turn) +
                      " role=" + record.role + " model=" + record.model +
                      " status=" + std::to_string(record.status);
    if (record.failed) out += " failed=1";
    // REQ-PREGEN-24: present only on a background call, so a foreground
    // record is byte-identical to the pre-pregeneration format. Placed after
    // status= and before the timing keys.
    if (record.background) out += " background=1";
    out += " namelookup_us=" + std::to_string(record.namelookupUs) +
           " connect_us=" + std::to_string(record.connectUs) +
           " appconnect_us=" + std::to_string(record.appconnectUs) +
           " starttransfer_us=" + std::to_string(record.starttransferUs) +
           " total_us=" + std::to_string(record.totalUs);
    // REQ-LAT-4: a failed call emits NO token keys — never a fabricated zero.
    // A 200 whose body carried no readable usage says so explicitly.
    if (!record.failed) {
        if (record.tokensKnown) {
            out += " input_tokens=" + std::to_string(record.inputTokens) +
                   " output_tokens=" + std::to_string(record.outputTokens);
        } else {
            out += " tokens=unknown";
        }
    }
    return out;
}

void profileEmit(const StageRecord& record) {
    if (!g_enabled) return;
    write(formatStage(record));
}

void profileEmit(const CallRecord& record) {
    if (!g_enabled) return;
    write(formatCall(record));
}

std::string formatDwell(const DwellRecord& record) {
    return "twprof kind=dwell turn=" + std::to_string(record.turn) +
           " ms=" + formatMs(record.ms);
}

void profileEmit(const DwellRecord& record) {
    if (!g_enabled) return;
    write(formatDwell(record));
}

std::string formatPregen(const PregenRecord& record) {
    std::string out = "twprof kind=pregen turn=" + std::to_string(record.turn) +
                      " outcome=" + record.outcome;
    // Each key belongs to exactly one outcome; a miss carries none, so its
    // line is the bare four-field form above.
    if (record.ageTurns) out += " age_turns=" + std::to_string(*record.ageTurns);
    if (record.waitMs) out += " wait_ms=" + formatMs(*record.waitMs);
    if (record.runMs) out += " run_ms=" + formatMs(*record.runMs);
    return out;
}

void profileEmit(const PregenRecord& record) {
    if (!g_enabled) return;
    write(formatPregen(record));
}

ScopedStage::~ScopedStage() {
    const std::chrono::duration<double, std::milli> elapsed =
        std::chrono::steady_clock::now() - start_;
    profileEmit(StageRecord{stage_, profileCurrentTurn(), elapsed.count(),
                            nestedIn_});
}

ScopedDwell::~ScopedDwell() {
    const std::chrono::duration<double, std::milli> elapsed =
        std::chrono::steady_clock::now() - start_;
    // profileCurrentTurn() is the turn just completed: the next turn is not
    // numbered until runTurn calls profileNextTurn() (REQ-PREGEN-25).
    profileEmit(DwellRecord{profileCurrentTurn(), elapsed.count()});
}
