// The logging mechanism (see log.hpp for the contract). No call sites live
// here — every subsystem owns its own.
#include "log.hpp"

#include <unistd.h>  // dup, dup2 — the REQ-LOG-2 terminal duplicate

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <vector>

namespace {

// TEXTWORLD_LOG_LEVEL, case-insensitive. Unset, empty, or unrecognized => the
// REQ-LOG-18 default, silently (REQ-LOG-19): a typo must not cost the player a
// message on the game screen, which is the one thing this whole module exists
// to prevent.
LogLevel readLevelEnv() {
    const char* v = std::getenv("TEXTWORLD_LOG_LEVEL");
    if (v == nullptr || v[0] == '\0') return LogLevel::Info;
    std::string lowered;
    for (const char* p = v; *p != '\0'; ++p) {
        lowered += static_cast<char>(
            std::tolower(static_cast<unsigned char>(*p)));
    }
    if (lowered == "error") return LogLevel::Error;
    if (lowered == "warn") return LogLevel::Warn;
    if (lowered == "info") return LogLevel::Info;
    if (lowered == "debug") return LogLevel::Debug;
    return LogLevel::Info;
}

// Cached at static-init: the getenv happens once per process (REQ-LOG-19). No
// other translation unit reads this during ITS static init, so the
// initialization order is unobservable.
LogLevel g_threshold = readLevelEnv();

// Process-local turn sequence. ATOMIC because both worker threads read it via
// logCurrentTurn() to stamp their entries while the main thread advances it
// between turns (REQ-LOG-13). Only the main thread ever writes.
std::atomic<int64_t> g_turn{0};

// REQ-LOG-12. Stored, not copied: callers pass string literals.
thread_local const char* g_threadName = "main";

// The logger is INERT until something gives it somewhere to write — logInit()
// opening the session file, or logSetSink() installing a capture. This is what
// makes REQ-LOG-28 fall out of the build graph rather than need a flag: the
// test binary links no main.cpp, so nothing calls logInit(), and a test that
// wants entries asks for them by setting a sink.
//
// Two flags rather than one because they are independent, and because CLEARING
// a sink must return the test binary to inert. Were it a single latch, one
// logging test would leave every later test's migrated messages spraying onto
// the suite's real stderr.
std::atomic<bool> g_hasFile{false};
std::atomic<bool> g_hasSink{false};

bool active() { return g_hasFile.load() || g_hasSink.load(); }

// Empty => the default writer.
std::function<void(const std::string&)>& sink() {
    static std::function<void(const std::string&)> s;
    return s;
}

// Serializes the whole of write() — the sink lookup, the sink call, and the
// default fprintf alike (REQ-LOG-15). Entries come from three threads, and
// this is what makes one entry one ATOMIC line rather than three threads'
// bytes braided together. Note the default writer is a SINGLE fprintf, not a
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
    // stderr is the session log file by the time this runs in the real binary
    // (REQ-LOG-3 reopened it). Flushed per entry so a session killed mid-turn
    // still has everything up to that point (REQ-LOG-16).
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
}

// REQ-LOG-11: local wall-clock, millisecond precision, YYYY-MM-DD HH:MM:SS.mmm.
std::string formatTimestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = time_point_cast<seconds>(now);
    const auto ms = duration_cast<milliseconds>(now - secs).count();

    const std::time_t t = system_clock::to_time_t(secs);
    std::tm local{};
    localtime_r(&t, &local);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(ms));
    return buf;
}

// Left-align to a minimum width. A field WIDER than its column pushes the line
// out rather than being truncated — the columns are for reading, the two-space
// separator is what a parser keys on.
void appendPadded(std::string& out, const std::string& field, size_t width) {
    out += field;
    for (size_t i = field.size(); i < width; ++i) out += ' ';
}

// The REQ-LOG-2 terminal duplicate: the original error channel, captured before
// any redirect and held for the life of the process. Nothing but
// logToTerminal() ever writes to it.
FILE* g_terminal = nullptr;

// REQ-LOG-5: textworld-YYYYMMDD-HHMMSS.log. Checked by hand rather than by
// <regex> — this runs on every file in the directory at startup, and the
// pattern is fixed.
bool isSessionLogName(const std::string& name) {
    static const std::string prefix = "textworld-";
    static const std::string suffix = ".log";
    if (name.size() != prefix.size() + 8 + 1 + 6 + suffix.size()) return false;
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    for (size_t i = prefix.size(); i < name.size() - suffix.size(); ++i) {
        const char c = name[i];
        if (i == prefix.size() + 8) {
            if (c != '-') return false;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// REQ-LOG-5, again: local time, year first, so a directory listing sorts
// chronologically by name.
std::string sessionFileName() {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_r(&t, &local);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "textworld-%04d%02d%02d-%02d%02d%02d.log",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return buf;
}

// REQ-LOG-9. Recency by MODIFICATION TIME, not by parsing the name, so files
// whose names collide at the one-second resolution of REQ-LOG-5 still order
// deterministically; the name breaks a tie in the timestamps. Only files
// matching the REQ-LOG-5 pattern are eligible — an unrelated file in the
// directory is never removed. Every step swallows its failures: a directory
// that cannot be enumerated, or a file that cannot be deleted, costs the
// session nothing and says nothing (REQ-LOG-7).
void applyRetention(const std::filesystem::path& dir, size_t keep) {
    namespace fs = std::filesystem;
    std::vector<std::pair<fs::file_time_type, fs::path>> logs;
    try {
        for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
            std::error_code ec;
            if (!entry.is_regular_file(ec) || ec) continue;
            if (!isSessionLogName(entry.path().filename().string())) continue;
            const fs::file_time_type mtime = fs::last_write_time(entry, ec);
            if (ec) continue;
            logs.emplace_back(mtime, entry.path());
        }
    } catch (const std::exception&) {
        return;
    }
    if (logs.size() <= keep) return;

    std::sort(logs.begin(), logs.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;  // newest first
        return a.second > b.second;
    });
    for (size_t i = keep; i < logs.size(); ++i) {
        std::error_code ec;
        fs::remove(logs[i].second, ec);  // ec swallowed: best-effort
    }
}

}  // namespace

const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Info: return "INFO";
        case LogLevel::Debug: return "DEBUG";
    }
    return "INFO";
}

bool logEnabled(LogLevel level) {
    return static_cast<int>(level) <= static_cast<int>(g_threshold);
}

void logRefreshLevel() { g_threshold = readLevelEnv(); }

void logSetThreadName(const char* name) { g_threadName = name; }

int64_t logNextTurn() { return ++g_turn; }

int64_t logCurrentTurn() { return g_turn; }

std::string formatEntry(const LogEntry& entry) {
    std::string out;
    out.reserve(96 + entry.message.size());
    out += formatTimestamp();  // fixed 23 chars
    out += "  ";
    appendPadded(out, logLevelName(entry.level), 5);
    out += "  ";
    appendPadded(out, entry.thread, 6);
    out += "  ";
    appendPadded(out, "turn=" + std::to_string(entry.turn), 7);
    out += "  ";
    appendPadded(out, entry.source, 6);
    out += "  ";
    out += entry.message;
    return out;
}

void logEmit(LogLevel level, const char* source, const std::string& message) {
    if (!active() || !logEnabled(level)) return;
    write(formatEntry(
        LogEntry{level, g_threadName, g_turn.load(), source, message}));
}

void logEmitf(LogLevel level, const char* source, const char* fmt, ...) {
    if (!active() || !logEnabled(level)) return;
    char buf[2048];
    std::va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    logEmit(level, source, buf);
}

LogInit logInit(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    LogInit result;

    // REQ-LOG-29 step 1, and it must be first: after step 4 there is no other
    // way back to the screen. Captured once — a second logInit() in one process
    // (only a test does that) must not leak a descriptor or re-capture a
    // channel that is by then the log file.
    if (g_terminal == nullptr) {
        const int copy = dup(fileno(stderr));
        if (copy >= 0) g_terminal = fdopen(copy, "w");
    }

    // Step 3. A failure to create the directory is not fatal on its own — the
    // fopen below simply fails too, and takes the same path.
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / sessionFileName();

    // Step 4. freopen on failure has ALREADY closed the stream (the C
    // standard), so there is no leaving it where it was: the only choices are
    // the file or the discard sink. Aiming it back at the terminal is exactly
    // what REQ-LOG-8 forbids.
    if (std::freopen(path.string().c_str(), "a", stderr) != nullptr) {
        result.fileOpen = true;
        result.path = path;
        g_hasFile = true;
    } else {
        std::freopen("/dev/null", "w", stderr);
        return result;  // no file => nothing to apply retention to
    }

    applyRetention(dir, 20);  // step 5 — the new file counts toward the 20
    return result;
}

void logShutdown() {
    std::fflush(stderr);
    if (g_terminal != nullptr) dup2(fileno(g_terminal), fileno(stderr));
    g_hasFile = false;
}

void logToTerminal(const char* fmt, ...) {
    FILE* out = g_terminal != nullptr ? g_terminal : stderr;
    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(out, fmt, args);
    va_end(args);
    std::fflush(out);
}

void logExempt(LogLevel level, const char* source, const char* fmt, ...) {
    char buf[2048];
    std::va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // The terminal copy first: it is the one that must survive even when the
    // log never opened, and it carries the message's own line breaks.
    logToTerminal("%s", buf);

    // The log copy, flattened — one entry is one line. Trailing whitespace goes
    // with it, so the common "…\n" message does not end in a stray space.
    std::string flat(buf);
    for (char& c : flat) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    while (!flat.empty() && flat.back() == ' ') flat.pop_back();
    logEmit(level, source, flat);
}

void logSetSink(std::function<void(const std::string&)> s) {
    const std::lock_guard<std::mutex> lock(sinkMutex());
    sink() = std::move(s);
    g_hasSink = static_cast<bool>(sink());
}
