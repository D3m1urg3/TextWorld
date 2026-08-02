// Background room pre-generation: the job, the candidate store, and the one
// worker thread that turns the former into the latter.
//
// WHY THIS IS ITS OWN TRANSLATION UNIT (REQ-PREGEN-7). Walking a latent exit
// is the game's worst turn — the architect's generation call sits on the
// critical path and costs ~7.3 s. This unit moves that call off the turn by
// running it while the player reads the previous room. The goal is REMOVING A
// VARIANCE SPIKE, not lowering the mean: an ordinary turn is untouched.
//
// THE DATABASE BOUNDARY IS THE WHOLE DESIGN. The worker runs on a second
// thread, and the engine's single-writer invariant is not negotiable, so this
// unit performs NO database access of any kind. Every read a job needs is done
// on the MAIN thread at queue time and snapshotted into the job (REQ-PREGEN-5).
// Two mechanical checks enforce it, and both are part of the build's
// validation:
//
//     grep -En "INSERT|UPDATE|DELETE|SELECT" src/pregen.cpp   -> empty
//     grep -n  "Db" src/pregen.hpp src/pregen.cpp             -> no parameter,
//                                                                no member
//
// If a change here ever wants a Db, that is the signal the work belongs in
// architect.cpp (which owns the snapshot: architectQueuePregen) rather than a
// signal to relax the check. The worker produces TEXT; every allocation of a
// scarce or unique thing still happens on the main thread at commit.
//
// WHAT LIVES WHERE:
//   * architect.cpp  — the scheduler: which exits to queue, and the reads that
//                      snapshot them (it is contractually read-only).
//   * pregen.cpp     — this: the store, the queue, the thread, the transport
//                      call, and the gate.
//   * systems.cpp    — the tick's side: pregenAcquire on a latent-exit walk,
//                      then architectCommitProposal for a candidate that
//                      arrived.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "architect.hpp"  // RoomProposal + the pure helpers the worker calls
#include "prose.hpp"      // HttpResponse / HttpTransport — the seam, UNCHANGED

// One unit of background work, fully self-contained: everything the worker
// needs, snapshotted on the main thread at queue time (REQ-PREGEN-5). NO
// database handle and NO pointer into world state — that absence is the
// invariant this struct exists to make structural rather than remembered.
struct PregenJob {
    int64_t room = 0;                      // the ORIGIN room, for keying only
    std::string direction;                 // the latent exit being prefetched
    std::string contextPayload;            // buildArchitectContext, snapshotted
    std::vector<std::string> enemyBlurbs;  // eligibleEnemyBlurbs, snapshotted
    int64_t snapshotTurn = 0;              // meta.turn when the snapshot was
                                           // taken; RECORDED AND REPORTED ONLY
                                           // (REQ-PREGEN-13) — it invalidates
                                           // nothing today
};

// The four states of a key, kept distinct because the tick treats Queued and
// Running DIFFERENTLY (REQ-PREGEN-12, REQ-PREGEN-16).
enum class PregenState {
    Absent,   // never queued, or a job failed and the slot was cleared
    Queued,   // a job exists; the worker has not started its transport call
    Running,  // the worker is executing this job's transport call now
    Ready,    // a validated RoomProposal is stored and committable
};

// What the tick actually got, one per latent-exit walk (REQ-PREGEN-23).
enum class PregenOutcome {
    Hit,        // a candidate was waiting: no network on the turn at all
    Waited,     // the worker was mid-call for THIS key; the tick waited it out
    RanQueued,  // the job was still queued: dequeued and run on the main thread
    Miss,       // nothing here — the tick falls back to today's synchronous path
};

// The canonical name of an outcome (REQ-PREGEN-23). These four strings ARE the
// vocabulary of the `outcome=` profile field; nothing else may appear there.
// Lives beside the enum so a fifth state has one obvious place to be named,
// rather than the name table drifting in whichever module happens to emit it.
const char* pregenOutcomeName(PregenOutcome outcome);

struct PregenResult {
    PregenOutcome outcome = PregenOutcome::Miss;
    // Present only when a candidate was produced. A Waited or RanQueued whose
    // job FAILED carries its outcome with no proposal — the tick then takes
    // the ordinary synchronous path, exactly as on a Miss.
    std::optional<RoomProposal> proposal;
    double waitMs = 0.0;       // Waited only
    double runMs = 0.0;        // RanQueued only
    int64_t snapshotTurn = 0;  // Hit only, for the staleness report
};

// TEXTWORLD_PREGEN, following the TEXTWORLD_AI convention exactly: ON by
// default, off only when the variable is set to EXACTLY "0" (REQ-PREGEN-1).
// The getenv happens ONCE per process into a file-static bool.
//
// Off means off completely: no thread, no job, no store activity, and no
// behavioral delta of any kind.
bool pregenEnabled();

// TEST-ONLY. Re-reads TEXTWORLD_PREGEN into the cache, mirroring
// profileRefreshEnabled(). Production code never calls this.
void pregenRefreshEnabledForTest();

// Hand one job to the worker. Called on the MAIN thread by the scheduler
// (architectQueuePregen) after the tick's transaction has committed and after
// the player's text has been flushed, so it can never delay the turn they
// waited on. No-op when pre-generation is off, and no-op when the key is
// already known — one candidate per key, ever (REQ-PREGEN-11, REQ-PREGEN-21).
void pregenSubmit(PregenJob job);

// The state of one key. Safe from either thread.
PregenState pregenStateOf(int64_t room, const std::string& direction);

// Start the one background worker (REQ-PREGEN-6). Idempotent. A NO-OP unless
// BOTH pregenEnabled() and architectEnabled() hold: with the architect off
// there are no generatable exits, so there is nothing for a thread to do and
// none is created (REQ-PREGEN-1, REQ-PREGEN-2).
//
// EXACTLY ONE thread, processing at most ONE job at a time. Concurrent
// in-flight generation is deliberately not introduced: coherent story
// generation — planned, out of scope here — cannot author rooms in ignorance
// of one another, and a serial worker is what can later be taught to consult
// shared story state without races.
//
// Must be called AFTER aiHttpInit(); the worker constructs its own easy handle
// inside its thread body, and curl_global_init must not race it.
void pregenStart();

// Signal the worker to stop, wake it, and JOIN it. Idempotent, and safe when
// no thread was ever started.
//
// ORDERING IS LOAD-BEARING (REQ-PREGEN-19): this must complete before
// aiHttpShutdown() reaches curl_global_cleanup(). Nothing calls it explicitly
// in production — PregenGuard, declared below AiHttpGuard in main(), gets the
// ordering from reverse destruction on EVERY exit path, including the error
// ones. Add a second call site and you have two things to keep in step.
//
// An in-flight transfer does not hold this up for the rest of its 8 s timeout:
// the stop flag is the worker client's abort flag, so libcurl abandons the
// transfer at its next progress callback (REQ-PREGEN-20).
void pregenStop();

// TEST HOOK for REQ-PREGEN-2. The spec asks for this explicitly rather than
// accepting "no pregen records in the log" as evidence: with no latent exits
// queued the log is empty whether the thread was never created or was created
// and left idle, so the absence of records proves nothing.
bool pregenWorkerRunning();

// TEST-ONLY. Installs a transport the worker uses INSTEAD of constructing an
// AiHttpWorkerClient, so the concurrency is exercised with no libcurl and no
// network at all. Set it before pregenStart(). An empty function restores the
// production behavior.
void pregenSetWorkerTransportForTest(HttpTransport transport);

// THE ONE CALL THE TICK MAKES, on a latent-exit walk. Resolves the key's state
// into a result the tick can act on, and is the only place the tick may block:
//
//   Ready   -> Hit,       immediately, no network
//   Running -> Waited,    waits out THAT job (bounded by the 8 s transport
//                         timeout); never starts a second call for the same key
//   Queued  -> RanQueued, removes the job from the queue and runs it HERE, on
//                         the main thread, reusing the snapshot. Deliberately
//                         NOT a wait for the worker to reach it: with one
//                         serial worker that would bound the wait at
//                         (queue depth x 8 s), which is the failure this whole
//                         design exists to avoid (REQ-PREGEN-16).
//   Absent  -> Miss,      the tick falls back to synchronous generation
//
// `transport` overrides the production main-thread transport on the two
// synchronous paths; tests inject here, exactly as resolve() already threads
// one through. Pass nullptr in production.
//
// Returns Miss immediately, touching nothing, when pre-generation is off.
PregenResult pregenAcquire(int64_t room, const std::string& direction,
                           const HttpTransport* transport);

// --- test-only hooks --------------------------------------------------------
// Named for what they are. Production code calls none of these.

// Stops the worker if one is running, then drops every slot and every pending
// job. Lets one process run many independent store scenarios.
void pregenResetForTest();

// Plant a ready candidate directly, as though a worker had produced it.
void pregenInjectReadyForTest(int64_t room, const std::string& direction,
                              const RoomProposal& proposal,
                              int64_t snapshotTurn);

// How many jobs are waiting in the queue right now.
std::size_t pregenPendingCountForTest();

// Process-lifetime RAII for the worker, mirroring AiHttpGuard.
//
// DECLARE IT AS main()'s SECOND LOCAL, immediately below the AiHttpGuard.
// Reverse destruction then gives join-before-curl_global_cleanup on EVERY exit
// path the binary has — normal return, quit, EOF, SchemaMismatch, and both
// catches — without a single explicit call (REQ-PREGEN-19). That ordering is
// not optional; violating it is undefined behavior, which is exactly the kind
// of thing that should fall out of the code's shape rather than out of anyone
// remembering it.
struct PregenGuard {
    PregenGuard() { pregenStart(); }
    ~PregenGuard() { pregenStop(); }

    PregenGuard(const PregenGuard&) = delete;
    PregenGuard& operator=(const PregenGuard&) = delete;
};

// How many threads are currently blocked inside pregenAcquire waiting out a
// Running job. Exists so a test can release the worker only once the tick has
// PROVABLY reached the wait: releasing earlier would let the job finish first
// and turn a `waited` into a `hit`, which is a flaky test rather than a
// different behavior. In production this is always 0 or 1 — one tick, one wait.
std::size_t pregenWaitingCountForTest();
