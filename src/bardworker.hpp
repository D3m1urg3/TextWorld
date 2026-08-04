// The bard's wake worker: the job, the one-slot state machine, and the second
// background thread that turns the former into a validated proposal.
//
// WHY THIS IS ITS OWN TRANSLATION UNIT (REQ-BARD-WAKE-19), and why the split is
// the same one pregen/architect already make. Waking the bard costs an Opus
// call. Putting that call on the turn would make every irreversible event —
// every kill, every generated room — the game's worst turn, which is precisely
// the variance spike pregen.cpp exists to remove. So the call runs on a second
// thread while the player plays on.
//
// THE DATABASE BOUNDARY IS THE WHOLE DESIGN. The engine's single-writer
// invariant is not negotiable, so this unit performs NO database access of any
// kind. Every read a wake needs is done on the MAIN thread at queue time and
// snapshotted into the job as TEXT. Two mechanical checks enforce it, and both
// are part of the build's validation:
//
//     grep -En "INSERT|UPDATE|DELETE|SELECT" src/bardworker.cpp  -> empty
//     grep -n  "Db" src/bardworker.hpp src/bardworker.cpp        -> no parameter,
//                                                                  no member
//
// If a change here ever wants a Db, that is the signal the work belongs in
// bard.cpp (which owns the snapshot, the stamp, and the commit) rather than a
// signal to relax the check. The worker produces a PROPOSAL; every write it
// implies still happens on the main thread, inside the main thread's
// transaction, at commit.
//
// WHAT LIVES WHERE:
//   * bard.cpp       — everything that touches the database: the overture, the
//                      trigger query, the snapshot, the queue-time stamp, and
//                      the commit of a ready result.
//   * bardworker.cpp — this: the job, the state machine, the dirty flag, the
//                      thread, the transport call, and the gate.
//   * main.cpp       — the two call sites: bardOverture at creation, and
//                      bardAfterTurn in the loop tail.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bard.hpp"   // WakeProposal + validateWakeResponse — the pure gate
#include "prose.hpp"  // HttpResponse / HttpTransport — the seam, UNCHANGED

// One wake, fully self-contained: everything the worker needs, snapshotted on
// the main thread at queue time. NO database handle and NO pointer into world
// state — that absence is the invariant this struct exists to make structural
// rather than remembered. What crosses the thread boundary is TEXT.
struct BardJob {
    std::string requestBody;           // buildWakeRequestBody, already finished
    std::vector<std::string> motives;  // the vocabulary the gate checks against;
                                       // it rides along because
                                       // validateWakeResponse takes it
    int64_t snapshotTurn = 0;          // meta.turn at queue time; RECORDED AND
                                       // REPORTED ONLY — it invalidates nothing
};

// The three states of the ONE slot. There is deliberately no Queued: pregen
// distinguishes it because its tick treats Queued and Running differently, and
// nothing in the bard ever waits on that distinction — there is one wake, one
// worker, and no acquire path. Idle -> Running -> Ready -> Idle, plus a flag.
enum class BardState {
    Idle,     // nothing in flight, nothing waiting to be taken
    Running,  // the worker is executing a wake's transport call now
    Ready,    // a validated WakeProposal is stored and committable
};

// TEXTWORLD_BARD, following the TEXTWORLD_AI convention exactly: ON by default,
// off only when the variable is set to EXACTLY "0" (REQ-BARD-WAKE-20). The
// getenv happens ONCE per process into a file-static bool.
//
// Off means off completely: no overture, no thread, no wake, and no behavioral
// delta of any kind.
bool bardEnabled();

// TEST-ONLY. Re-reads TEXTWORLD_BARD into the cache, mirroring
// pregenRefreshEnabledForTest. Production code never calls this.
void bardRefreshEnabledForTest();

// The state of the slot. Safe from either thread.
BardState bardStateNow();

// Hand one wake to the worker. MAIN THREAD ONLY. Idle -> Running, returning
// true. Otherwise the slot is busy: nothing is queued, the DIRTY FLAG is set,
// and this returns false (REQ-BARD-WAKE-13, -14). There is no queue behind
// this — at most ONE wake is ever in flight process-wide, and a wake that could
// not be sent is remembered as a flag rather than as a backlog. Only the main
// thread submits and only the worker leaves Running, so the Idle the caller
// observed cannot be invalidated underneath it.
bool bardSubmit(BardJob job);

// Set the dirty flag without submitting: "a trigger fired while the bard was
// busy." MAIN THREAD ONLY.
void bardSetDirty();

// CONSUME the dirty flag: returns whether it was set, and clears it.
// MAIN THREAD ONLY. Read on every evaluation rather than only when busy, so it
// can never outlive the evaluation that should have answered it.
//
// The flag is BELT TO THE QUERY'S BRACES, and should be read as such: because
// meta.bard_last_wake_turn is stamped at QUEUE time, the trigger query re-finds
// the events that arrived mid-wake all by itself. The flag is what makes
// REQ-BARD-WAKE-14's "exactly one further evaluation" true even when the rate
// ceiling would otherwise have swallowed it.
bool bardTakeDirty();

// Ready -> Idle, handing the proposal over. nullopt when nothing is ready,
// which is the common case. MAIN THREAD ONLY: the caller is about to commit it.
std::optional<WakeProposal> bardTakeReady();

// Start the one wake worker (REQ-BARD-WAKE-16). Idempotent. A NO-OP unless BOTH
// bardEnabled() and aiNarrationEnabled() hold: with AI off there is nothing to
// call, so no thread is created — not a thread that sits idle.
//
// Must be called AFTER aiHttpInit(); the worker constructs its own easy handle
// inside its thread body, and curl_global_init must not race it.
void bardStart();

// Signal the worker to stop, wake it, and JOIN it. Idempotent, and safe when no
// thread was ever started.
//
// ORDERING IS LOAD-BEARING (REQ-BARD-WAKE-18): this must complete before
// aiHttpShutdown() reaches curl_global_cleanup(). Nothing calls it explicitly
// in production — BardGuard, declared below AiHttpGuard in main(), gets the
// ordering from reverse destruction on EVERY exit path, including the error
// ones.
//
// An in-flight transfer does not hold this up for the rest of its timeout: the
// stop flag is the worker client's abort flag, so libcurl abandons the transfer
// at its next progress callback (REQ-BARD-WAKE-20). A result that had already
// become Ready is simply DROPPED (REQ-BARD-WAKE-25) — a wake is never persisted
// across sessions, and the next session's first irreversible event queues a
// fresh one against a world that has meanwhile moved on.
void bardStop();

// TEST HOOK for REQ-BARD-WAKE-20. Asked for explicitly rather than accepting
// "no bard records in the log" as evidence: with no triggers fired the log is
// empty whether the thread was never created or was created and left idle, so
// the absence of records proves nothing.
bool bardWorkerRunning();

// TEST-ONLY. Installs a transport the worker uses INSTEAD of constructing an
// AiHttpWorkerClient, so the concurrency is exercised with no libcurl and no
// network at all. Set it before bardStart(). An empty function restores the
// production behavior.
void bardSetWorkerTransportForTest(HttpTransport transport);

// TEST-ONLY. Stops the worker if one is running, then clears the slot, the
// dirty flag, and the injected transport. Lets one process run many independent
// scenarios.
void bardResetForTest();

// TEST-ONLY. Blocks until no wake is in flight — that is, until the state is
// not Running — and returns.
//
// WHY THIS EXISTS AT ALL. The bard's tick NEVER blocks: unlike pregenAcquire,
// no production path waits on the worker, so there is no production waiter for
// a test to synchronize against. This is the drain wait that gives bard tests
// the same shape pregen's have — spawn a thread in here, spinUntil the count
// below reads 1, and only THEN release a blocking transport, so the release can
// never race the call it is meant to unblock. Releasing earlier would let the
// wake finish first and turn the thing being tested into an accident of timing.
void bardWaitForIdleForTest();

// How many threads are currently inside bardWaitForIdleForTest. Guarded by the
// same mutex as everything else; nothing in the production path branches on it.
std::size_t bardWaitingCountForTest();

// Process-lifetime RAII for the wake worker, mirroring PregenGuard.
//
// DECLARE IT BELOW AiHttpGuard in main(). Reverse destruction then gives
// join-before-curl_global_cleanup on EVERY exit path the binary has — normal
// return, quit, EOF, SchemaMismatch, and both catches — without a single
// explicit call (REQ-BARD-WAKE-18). That ordering is not optional; violating it
// is undefined behavior, which is exactly the kind of thing that should fall
// out of the code's shape rather than out of anyone remembering it.
//
// REQ-BARD-WAKE-18 places this below PregenGuard specifically, and a
// source-order test pins that — but the half that separates defined from
// undefined behavior is "below AiHttpGuard": the two workers are independent of
// each other and neither joins through the other.
struct BardGuard {
    BardGuard() { bardStart(); }
    ~BardGuard() { bardStop(); }

    BardGuard(const BardGuard&) = delete;
    BardGuard& operator=(const BardGuard&) = delete;
};
