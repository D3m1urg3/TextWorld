// Profiling mechanism (see profile.hpp for the contract). No call sites live
// here — loop.cpp, architect.cpp and aihttp.cpp own those.
#include "profile.hpp"

#include <cstdio>
#include <cstdlib>

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

// Process-local turn sequence (not meta.turn).
int64_t g_turn = 0;

// Empty => the default stderr sink.
std::function<void(const std::string&)>& sink() {
    static std::function<void(const std::string&)> s;
    return s;
}

void write(const std::string& line) {
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

ScopedStage::~ScopedStage() {
    const std::chrono::duration<double, std::milli> elapsed =
        std::chrono::steady_clock::now() - start_;
    profileEmit(StageRecord{stage_, profileCurrentTurn(), elapsed.count(),
                            nestedIn_});
}
