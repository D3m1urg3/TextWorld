// textworld: the game binary. Interface contract (spec): invoked with no
// arguments, reads commands from stdin, writes to stdout, and uses the fixed
// world file "world.db" in the current working directory.
#include <cstdio>
#include <iostream>
#include <string>

#include "loop.hpp"
#include "world.hpp"

int main() {
    try {
        Db db = openWorld("world.db");

        // Courtesy render before the first prompt: where you are. Read-only —
        // no tick, no transaction, no event row (REQ-PROTO-5).
        std::fputs(renderStartup(db).c_str(), stdout);

        std::string line;
        while (true) {
            std::fputs("> ", stdout);
            std::fflush(stdout);
            if (!std::getline(std::cin, line)) break;  // EOF behaves as quit

            const TurnResult result = runTurn(db, line);
            std::fputs(result.output.c_str(), stdout);
            if (result.outcome == TurnOutcome::Quit) break;
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
