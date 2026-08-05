// Session logging: the one route internal engine messages take (REQ-LOG-1).
// The terminal is the game screen — game text goes to stdout, and everything
// the engine has to say about itself comes here instead.
//
// Every entry is ONE line of six fields in fixed order (REQ-LOG-10), each
// left-aligned to a minimum width and separated by two spaces, so a file reads
// as columns without becoming unparseable when a field overflows:
//
//   2026-08-05 14:30:22.184  INFO   main    turn=0   world   opened world.db
//   2026-08-05 14:30:41.902  DEBUG  bard    turn=3   bard    rejected: no motive
//   2026-08-05 14:30:44.310  WARN   main    turn=3   prose   using template text
//   2026-08-05 14:31:02.771  ERROR  pregen  turn=7   pregen  job failed: timeout
//
// THREAD SAFETY (REQ-LOG-15). THREE threads emit: the main game, the
// pre-generation worker, and the bard worker. Emission is serialized under a
// file-static mutex and one entry is one write call, so an entry is never
// interleaved with another's bytes and never truncated. This is the invariant
// profile.cpp held for two threads, widened to three — keep it when adding a
// sink or a call site.
//
// PRIVACY (REQ-LOG-26): no entry at any level may carry an API key, and no
// entry at INFO or above may carry a prompt, a response body, or the player's
// typed input. A rejection REASON ("motive not in vocabulary") is neither a
// response body nor player text, and is allowed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// Ordered most severe to least. The threshold admits every level at or above
// it, so DEBUG is the widest setting and ERROR the narrowest (REQ-LOG-17).
enum class LogLevel { Error, Warn, Info, Debug };

// The name as it appears in the level column: "ERROR", "WARN", "INFO", "DEBUG".
const char* logLevelName(LogLevel level);

// TEXTWORLD_LOG_LEVEL, case-insensitive, read ONCE per process and cached at
// static-init (REQ-LOG-19) — this is a plain enum comparison on the hot path,
// which is what keeps REQ-LOG-25's budget intact. Unset, empty, or unrecognized
// falls back to INFO with no complaint (REQ-LOG-18).
bool logEnabled(LogLevel level);

// TEST-ONLY. Re-reads TEXTWORLD_LOG_LEVEL into the cache so a test can flip the
// threshold inside one process. Production code never calls this — it is the
// mirror of the profileRefreshEnabled() this module absorbed.
void logRefreshLevel();

// The thread column (REQ-LOG-12). thread_local, defaulting to "main", so only
// the two worker threads need to say anything. `name` must outlive the thread
// (a string literal in practice) — it is stored, not copied.
void logSetThreadName(const char* name);

// Process-local turn sequence, so every entry of one turn shares a `turn=`
// value (REQ-LOG-13). Independent of meta.turn — no database access on the
// turn path. The counter is atomic, so logCurrentTurn() is safe from ANY
// thread while the main thread advances it; only the main thread ever calls
// logNextTurn(). profile.cpp's profileNextTurn/profileCurrentTurn forward here,
// which keeps the dependency one-way: profile -> log.
int64_t logNextTurn();
int64_t logCurrentTurn();

// One entry's six fields before formatting. `thread` and `source` are string
// literals owned by the caller (never freed here).
struct LogEntry {
    LogLevel level = LogLevel::Info;
    const char* thread = "main";
    int64_t turn = 0;
    const char* source = "";
    std::string message;
};

// Pure formatter — one line, NO trailing newline. Pure so the entry format
// (REQ-LOG-10, -11) is testable without capturing a stream or a file.
std::string formatEntry(const LogEntry& entry);

// Format + write one entry, stamped with the calling thread's name and the
// current turn. No-op unless the level passes the threshold AND the logger is
// active (a file is open or a sink is set) — so the test binary, which calls
// neither logInit() nor logSetSink(), writes nothing anywhere (REQ-LOG-28).
void logEmit(LogLevel level, const char* source, const std::string& message);

// printf-shaped logEmit. Exists so a migrated call site keeps its existing
// format string verbatim, with only the source prefix lifted out of it into
// the `source` column (REQ-LOG-14).
//
// The formatted message is bounded at 2 KB and TRUNCATED past it, silently. No
// diagnostic this logger carries comes close — the longest interpolate an
// exception message or a handle — and a bound is what keeps one entry one
// stack-allocated line, which is what REQ-LOG-15's single write rests on.
void logEmitf(LogLevel level, const char* source, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

// What logInit() managed to do. `fileOpen` false means logging is off for the
// session and `path` is meaningless — the game plays on regardless (REQ-LOG-7).
struct LogInit {
    bool fileOpen = false;
    std::filesystem::path path;
};

// Open this session's log and point the error channel at it. Called ONCE, as
// the first statement of main(), and never from the test binary — which is
// what keeps the REQ-LOG-3 backstop out of the suite (REQ-LOG-28). Performs
// REQ-LOG-29 steps 1 and 3-5, in that order:
//
//   1. duplicate the original terminal error channel and hold it for the life
//      of the process (REQ-LOG-2) — BEFORE anything can redirect it;
//   3. create `dir` if absent and open textworld-YYYYMMDD-HHMMSS.log in it;
//   4. redirect the error channel there, or to a discard sink if the open
//      failed, so a failure never leaves it aimed at the game screen
//      (REQ-LOG-3, -8);
//   5. delete all but the 20 most recent matching files, the new one included
//      (REQ-LOG-9).
//
// BEST-EFFORT throughout (REQ-LOG-7): every filesystem failure is swallowed.
// Nothing here retries, reports, or exits non-zero. `dir` is a parameter rather
// than the literal "logs" so a test can drive this against a temp directory.
LogInit logInit(const std::filesystem::path& dir);

// Undo logInit(): flush, and point the error channel back at the terminal
// duplicate. Production has no need of it — the process is ending — but a test
// that exercises logInit() must not leave the suite's stderr aimed at a temp
// file for every test that follows.
void logShutdown();

// The ONLY sanctioned route to the game screen besides game text on stdout
// (REQ-LOG-2), and the reason no other unit in `src/` writes to the error
// channel directly — a source-text guard in the suite pins that. Writes to the
// duplicate captured by logInit() step 1, or to the real error channel when
// there is none (the test binary, which never calls logInit). Structurally
// independent of whether the log file opened, so a failed log creation can
// never swallow the two exempt messages (REQ-LOG-8).
void logToTerminal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// A REQ-LOG-2 exempt message: one call, BOTH channels. The terminal gets the
// text exactly as written, newlines and all; the log gets the same text
// flattened to one line, because an entry is one line (REQ-LOG-10).
//
// This exists so the pairing is the logger's rule rather than each caller's.
// There are exactly two exemptions — the schema-mismatch refusal and the fatal
// exception — and writing them by hand as a logToTerminal + logEmitf pair left
// the two channels' wording free to drift apart with nothing to catch it.
// Anything that is NOT one of those two exemptions must use logEmit instead:
// reaching the screen at all is the thing REQ-LOG-1 forbids.
void logExempt(LogLevel level, const char* source, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Redirect entries (REQ-LOG-27). The sink receives the FORMATTED six-field
// line without its newline — exactly the shape profileSetSink used, so a test
// observes what the file would have held. An empty function restores the
// default writer. Setting a sink also makes the logger active.
void logSetSink(std::function<void(const std::string&)> sink);
