// Profiling mechanism (see profile.hpp for the contract). No call sites live
// here — loop.cpp, architect.cpp and aihttp.cpp own those.
//
// Since REQ-LOG-22 this unit owns FORMAT ONLY. The gate, the serialization, the
// sink and the writing all belong to log.cpp: a timing record is a DEBUG log
// entry whose message is the `twprof key=value` payload, carried byte-for-byte
// after the standard six-field prefix (REQ-LOG-23). The four format* functions
// below are therefore untouched by that move, which is what makes the payload
// identity checkable rather than merely intended.
#include "profile.hpp"

#include <cstdio>

#include "log.hpp"  // the gate, the turn counter, and the emission path

namespace {

// Milliseconds with three decimals — microsecond resolution, no exponent.
std::string formatMs(double ms) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", ms);
    return buf;
}

}  // namespace

// One gate for the whole engine now (REQ-LOG-22): the old profiling variable
// is retired and TEXTWORLD_LOG_LEVEL=debug is the single switch. Still a cached enum
// comparison, so REQ-LAT-1's overhead budget — and REQ-LOG-25's restatement of
// it — is unchanged.
bool profilingEnabled() { return logEnabled(LogLevel::Debug); }

void profileRefreshEnabled() { logRefreshLevel(); }

// The counter itself lives in log.cpp: REQ-LOG-13 makes it a logging concern,
// and profile.cpp depends on the logger, so keeping it here would make the
// dependency circular. These two survive as forwarders so every existing call
// site and test compiles untouched.
int64_t profileNextTurn() { return logNextTurn(); }

int64_t profileCurrentTurn() { return logCurrentTurn(); }

// Kept under its own name so no profiling call site had to move. What it
// installs is the LOGGER's sink, which receives the whole formatted line —
// six-field prefix and twprof payload together (REQ-LOG-27).
void profileSetSink(std::function<void(const std::string&)> s) {
    logSetSink(std::move(s));
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
    if (!logEnabled(LogLevel::Debug)) return;  // REQ-LOG-25: no formatting below
    logEmit(LogLevel::Debug, "profile", formatStage(record));
}

void profileEmit(const CallRecord& record) {
    if (!logEnabled(LogLevel::Debug)) return;  // REQ-LOG-25: no formatting below
    logEmit(LogLevel::Debug, "profile", formatCall(record));
}

std::string formatDwell(const DwellRecord& record) {
    return "twprof kind=dwell turn=" + std::to_string(record.turn) +
           " ms=" + formatMs(record.ms);
}

void profileEmit(const DwellRecord& record) {
    if (!logEnabled(LogLevel::Debug)) return;  // REQ-LOG-25: no formatting below
    logEmit(LogLevel::Debug, "profile", formatDwell(record));
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
    if (!logEnabled(LogLevel::Debug)) return;  // REQ-LOG-25: no formatting below
    logEmit(LogLevel::Debug, "profile", formatPregen(record));
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
