// textworld: the game binary. Interface contract (spec): invoked with no
// arguments, reads commands from stdin, writes to stdout, and uses the fixed
// world file "world.db" in the current working directory.
#include <cstdio>
#include <iostream>
#include <string>

#include "aihttp.hpp"
#include "architect.hpp"   // architectQueuePregen — the pre-generation scheduler
#include "bard.hpp"        // bardOverture — the one cold call, at creation
#include "bardworker.hpp"  // BardGuard — worker two
#include "loop.hpp"
#include "pregen.hpp"
#include "profile.hpp"    // ScopedDwell — how long the player took to answer
#include "prose.hpp"
#include "world.hpp"

int main() {
    try {
        // libcurl init/shutdown, once per process (REQ-LAT-7). First local in
        // the try, so its destructor covers every way out below: normal return,
        // quit, EOF, SchemaMismatch, and the generic catch.
        const AiHttpGuard httpGuard;

        auto world = openWorld("world.db");
        Db& db = world.db;

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

        std::string line;
        while (true) {
            std::fputs("> ", stdout);
            std::fflush(stdout);

            // The dwell timer (REQ-PREGEN-25) covers exactly the gap between
            // the prompt reaching the terminal and the player's line coming
            // back — the number that says whether one serial worker can keep
            // up with a reader. The getline is wrapped in its own block so the
            // timer destructs (and emits) BEFORE the break decision; the record
            // is emitted on the EOF path too, which is right, because that was
            // a real wait.
            bool eof = false;
            {
                const ScopedDwell dwell;
                eof = !std::getline(std::cin, line);
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
        // openWorld already printed the refusal message to stderr.
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
