// textworld: the game binary. Interface contract (spec): invoked with no
// arguments, reads commands from stdin, writes to stdout, and uses the fixed
// world file "world.db" in the current working directory.
#include <cstdio>
#include <iostream>
#include <string>

#include "aihttp.hpp"
#include "architect.hpp"  // architectQueuePregen — the pre-generation scheduler
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

        // The pre-generation worker. SECOND local, deliberately: reverse
        // destruction joins the thread BEFORE aiHttpShutdown() runs
        // curl_global_cleanup, on every one of those same exit paths
        // (REQ-PREGEN-19). This is the only pregenStart/pregenStop pair in the
        // binary — add a second and there are two things to keep in step.
        const PregenGuard pregenGuard;

        Db db = openWorld("world.db");

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
