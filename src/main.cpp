// textworld: the game binary. Interface contract (spec): invoked with no
// arguments, reads commands from stdin, writes to stdout, and uses the fixed
// world file "world.db" in the current working directory.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>  // isatty — REQ-POLISH-22's explicit branch

#include "aihttp.hpp"
#include "architect.hpp"   // architectQueuePregen — the pre-generation scheduler
#include "bard.hpp"        // bardOverture — the one cold call, at creation
#include "bardworker.hpp"  // BardGuard — worker two
#include "log.hpp"  // logInit — the first thing main() does (REQ-LOG-29)
#include "loop.hpp"
#include "pregen.hpp"
#include "profile.hpp"    // ScopedDwell — how long the player took to answer
#include "prose.hpp"
#include "term.hpp"  // currentStyle — the prompt's blank line is terminal-only
#include "world.hpp"

#include "linenoise.h"  // REQ-POLISH-20: line editing and history

namespace {

// REQ-LOG-21's session-end entry, written from a destructor so that every way
// out of main() reaches it: normal return, quit, EOF, SchemaMismatch, and the
// generic catch alike. The turn count is the process-local counter, which is
// what every other entry in the file is stamped with.
struct SessionLogGuard {
    ~SessionLogGuard() {
        logEmitf(LogLevel::Info, "main", "session end: %lld turns",
                 static_cast<long long>(logCurrentTurn()));
    }
};

// Read the hand-authored major-character files, as DATA for openWorld
// (REQ-NPCSTORE-34): main() does the file I/O, world.cpp does none of its own
// for these.
//
// An absent or unreadable directory yields an EMPTY vector, SILENTLY. No majors
// is a valid world (REQ-NPCSTORE-32), and `seed/majors/` does not exist in the
// shipped tree — the first authored major is content work, not this brick's.
//
// Sorted by filename, so catalog ids and the loader's duplicate-handle check
// are deterministic across platforms: directory_iterator's order is not.
//
// This runs on EVERY launch, including resumed ones where initialize() never
// runs and the vector is discarded. That is a few small file reads at startup,
// and it buys a call site with no branch in it — noted rather than optimised.
std::vector<MajorProfileFile> readMajorProfiles(const std::filesystem::path& dir) {
    std::vector<MajorProfileFile> files;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return files;

    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());

    for (const std::filesystem::path& p : paths) {
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;  // unreadable: silent, same as an absent directory
        std::ostringstream buf;
        buf << in.rdbuf();
        files.push_back({p.filename().string(), buf.str()});
    }
    return files;
}

}  // namespace

int main() {
    // REQ-LOG-29 steps 1-5, and they are FIRST: the terminal duplicate has to
    // be taken before anything can redirect the error channel, and the backstop
    // has to be in place before any code that might print. Best-effort — a
    // failure here is silent and the game plays on (REQ-LOG-7).
    const LogInit logFile = logInit("logs");
    const SessionLogGuard sessionLogGuard;

    // Step 6: the session-start entries, so a healthy run produces a file that
    // is visibly working rather than an empty one (REQ-LOG-21).
    logEmitf(LogLevel::Info, "main", "session start: logging to %s",
             logFile.path.string().c_str());
    logEmitf(LogLevel::Info, "main", "ai narration %s",
             aiNarrationEnabled() ? "on" : "off");

    try {
        // libcurl init/shutdown, once per process (REQ-LAT-7). First local in
        // the try, so its destructor covers every way out below: normal return,
        // quit, EOF, SchemaMismatch, and the generic catch.
        const AiHttpGuard httpGuard;

        auto world = openWorld("world.db", "seed/base.sql", "seed/setting.txt",
                               readMajorProfiles("seed/majors"));
        Db& db = world.db;
        logEmitf(LogLevel::Info, "world", "opened world.db (%s)",
                 world.created ? "created" : "resumed");

        // The overture (REQ-BARD-WAKE-2, -3): ONCE per world file, on the MAIN
        // thread, blocking, and before either worker thread exists. `created`
        // is the half of the condition only main() knows; the bard/AI half is
        // checked inside, first thing. Blocking is the point — generated rooms
        // are canon forever, so a story-less opening area would be permanent.
        // Never throws: a failure here leaves an empty catalog and today's game.
        if (world.created) bardOverture(db, nullptr);

        // The two background workers. What is load-bearing is that BOTH are
        // declared BELOW the AiHttpGuard: reverse destruction then joins both
        // threads BEFORE aiHttpShutdown() runs curl_global_cleanup, on every
        // one of the exit paths above — normal return, quit, EOF,
        // SchemaMismatch, and both catches (REQ-PREGEN-19, REQ-BARD-WAKE-18).
        // REQ-BARD-WAKE-18 also fixes their order relative to each other, and
        // a source-order test pins it — though only the "below AiHttpGuard"
        // half is what makes the difference between defined and undefined
        // behavior; the two workers themselves are independent. These are the
        // only start/stop pairs in the binary — add a call site and there are
        // two things to keep in step.
        const PregenGuard pregenGuard;
        const BardGuard bardGuard;

        // The title screen (REQ-POLISH-29): the FIRST thing on screen, before
        // the template-mode notice and before the first room. Static text from
        // the seed, so there is no renderer and no runtime dependency; below its
        // natural width it degrades to the game's name (REQ-POLISH-31), which is
        // also what a world file created before the row existed gets.
        std::fputs(titleScreen(db, detectWidth()).c_str(), stdout);

        // One-line mode notice (REQ-PROSE-2): told once, before the first
        // prompt, when AI narration is off. Silence means AI mode.
        if (!aiNarrationEnabled()) {
            std::fputs("AI narration off — template mode\n", stdout);
        }

        // Courtesy render before the first prompt: where you are. Read-only —
        // no tick, no transaction, no event row (REQ-PROTO-5).
        std::fputs(renderStartup(db).c_str(), stdout);

        // Queue point one: the starting room, before the first prompt
        // (REQ-PREGEN-4). By the time the player has read the room and typed
        // anything, a candidate for its latent exits may already be waiting.
        architectQueuePregen(db, playerRoom(db));

        // REQ-POLISH-22: line editing is for a TERMINAL, and the branch is on
        // isatty EXPLICITLY rather than on linenoise's own non-tty path. That is
        // what makes "piped behaviour is unchanged" structural: a piped run
        // executes the same std::getline it always did, so the captures under
        // .lore/work/validation/ cannot drift, and there is no editing, no
        // escape byte and no length limit on that path.
        const bool interactive = isatty(STDIN_FILENO) != 0;

        std::string line;
        while (true) {
            // REQ-POLISH-4: one blank line before each prompt, so turns are
            // visually separated. Emitted WITH the prompt rather than appended
            // to the turn's output, so no turn gains a trailing newline and
            // nothing downstream of runTurn changes.
            //
            // Gated on currentStyle().attrs — already false for a pipe and for
            // TERM=dumb, and the same flag REQ-POLISH-26 uses for the spinner.
            // An unconditional "\n> " would put a blank line into every piped
            // capture, which is the half of spec check 4 that forbids it.
            const char* prompt = currentStyle().attrs ? "\n> " : "> ";

            // The dwell timer (REQ-PREGEN-25) covers exactly the gap between
            // the prompt reaching the terminal and the player's line coming
            // back — the number that says whether one serial worker can keep
            // up with a reader. The read is wrapped in its own block so the
            // timer destructs (and emits) BEFORE the break decision; the record
            // is emitted on the EOF path too, which is right, because that was
            // a real wait. On the interactive path the prompt is linenoise's
            // to print, so it is INSIDE the timed block, which keeps the timer
            // covering exactly the same span as before.
            bool eof = false;
            {
                const ScopedDwell dwell;
                if (interactive) {
                    // linenoise returns malloc'd memory; the unique_ptr is what
                    // keeps the free off every return path (REQ-POLISH-20).
                    // NULL means EOF, which maps straight onto the existing
                    // break below (REQ-POLISH-23).
                    const std::unique_ptr<char, void (*)(void*)> input(
                        linenoise(prompt), linenoiseFree);
                    eof = input == nullptr;
                    if (!eof) line = input.get();
                } else {
                    std::fputs(prompt, stdout);
                    std::fflush(stdout);
                    eof = !std::getline(std::cin, line);
                }
            }
            if (eof) break;  // EOF behaves as quit, unchanged

            const TurnResult result = runTurn(db, line);
            std::fputs(result.output.c_str(), stdout);
            if (result.outcome == TurnOutcome::Quit) break;
            // Flush BEFORE queuing, so REQ-PREGEN-4's "after game output has
            // been written" is literally true rather than nearly true. Without
            // this the text would still be sitting in the buffer while the
            // scheduler ran its reads. Same bytes in the same order — only the
            // timing of the write syscall changes.
            std::fflush(stdout);

            // Queue point two: after EVERY non-quit turn, whatever its outcome
            // (REQ-PREGEN-4's condition is a property of the ROOM, not of the
            // turn). The tick has committed and the player already has their
            // text, so this can never delay the turn they waited on. Uniform
            // and idempotent: a room with nothing to queue queues nothing.
            architectQueuePregen(db, playerRoom(db));

            // The bard's one turn-loop call (REQ-BARD-WAKE-8): AFTER the tick's
            // transaction has committed and AFTER the fflush above, so neither
            // half of it — committing a wake that landed, or evaluating whether
            // to queue a new one — can delay the turn the player waited on.
            // Same rule and same call-site shape as architectQueuePregen.
            // Never throws: a broken bard costs the session nothing.
            bardAfterTurn(db);
        }
        return 0;
    } catch (const SchemaMismatch&) {
        // openWorld already wrote the refusal to the terminal duplicate and
        // to the log (REQ-LOG-2). Nothing is added here.
        return 1;
    } catch (const std::exception& e) {
        // The other REQ-LOG-2 exemption: the binary is already dying, so there
        // is no game on screen for this to intrude on. One call, both channels.
        logExempt(LogLevel::Error, "main", "fatal: %s\n", e.what());
        return 1;
    }
}
