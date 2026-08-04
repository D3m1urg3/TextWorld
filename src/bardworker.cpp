// See bardworker.hpp for the contract, and for the two grep checks that keep
// this file free of database access. Nothing in here takes a Db, and no SQL of
// any kind appears below — the job arrives pre-snapshotted from the main
// thread.
#include "bardworker.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "aihttp.hpp"  // makeAnthropicTransport + AiHttpWorkerClient
#include "prose.hpp"   // aiNarrationEnabled — the AI half of the gate

namespace {

// TEXTWORLD_BARD: ON unless set to exactly "0" (REQ-BARD-WAKE-20).
// Deliberately the TEXTWORLD_AI rule and not TEXTWORLD_PROFILE's — an unset
// variable means the feature is on, so a player who has never heard of it gets
// a world with a story in it.
bool readBardEnv() {
    const char* v = std::getenv("TEXTWORLD_BARD");
    if (v == nullptr) return true;
    return std::string(v) != "0";
}

// Cached at static-init time: the getenv happens once per process, so the turn
// path pays a bool read.
bool g_enabled = readBardEnv();

// --- the one slot ----------------------------------------------------------
//
// One mutex guards the whole state machine. It is NEVER held across a transport
// call — the worker drops it before calling and re-takes it after, which is the
// entire reason the main thread can read the state while a wake is in flight.

std::mutex g_mutex;

// Signalled after EVERY state transition and on every submit, so both the
// worker (waiting for work) and a test drain (waiting out a Running wake) wake
// on the same condition variable. One condvar for one mutex.
std::condition_variable g_cv;

// THE slot. Singular, deliberately: REQ-BARD-WAKE-13's "at most one wake in
// flight, process-wide" is a property of this being one variable rather than a
// container with a bound checked somewhere.
BardState g_state = BardState::Idle;

// The wake the worker has not yet picked up. Present between bardSubmit and the
// worker taking it; empty at every other moment. `g_state` is already Running
// for that whole window, so nothing outside this file can observe the gap.
std::optional<BardJob> g_job;

// The result of a finished wake, waiting for the main thread to commit it.
// Present exactly when g_state is Ready.
std::optional<WakeProposal> g_ready;

// "A trigger fired while the bard was busy" (REQ-BARD-WAKE-14). MAIN-THREAD
// state in practice, but guarded like everything else so the invariant is the
// mutex rather than a convention about who touches what.
bool g_dirty = false;

// How many threads are inside bardWaitForIdleForTest. Test observability only;
// nothing in the production path branches on it.
std::size_t g_waiting = 0;

// The worker's stop flag. Also handed to the worker's AiHttpWorkerClient as its
// abort flag, so setting it tears down an in-flight transfer through libcurl's
// progress callback rather than waiting out the timeout (REQ-BARD-WAKE-20).
// Atomic because libcurl polls it from inside perform().
std::atomic<bool> g_stopping{false};

// The worker itself. `g_worker` and `g_workerRunning` are touched ONLY by the
// main thread (bardStart / bardStop / bardResetForTest), which is what makes
// their lifetime reasoning a straight line rather than a protocol.
// `g_workerRunning` is still read under the mutex because the test hook may ask
// from anywhere.
std::thread g_worker;
bool g_workerRunning = false;

// TEST-ONLY. When set, the worker uses this INSTEAD of constructing an
// AiHttpWorkerClient — so the concurrency tests touch no libcurl at all.
HttpTransport g_testTransport;

// One wake, end to end, and deliberately synchronous so it is testable WITHOUT
// a thread. The transport call plus the PURE gate — no database, no retries.
//
// What is added here beyond the two calls is the catch, including the
// catch-all: a throwing transport must never escape onto the worker thread,
// where there is no caller to catch it and the process would die — which is the
// exact inverse of the degradation claim this whole brick is built to keep. The
// diagnostic says `bard` because a failed wake and a walled turn are different
// events to whoever is reading stderr.
std::optional<WakeProposal> runWake(const BardJob& job,
                                    const HttpTransport& transport) {
    try {
        // validateWakeResponse is total: a non-200, an unparseable body, or a
        // missing tool call all come back as nullopt rather than as a throw,
        // and a response calling NO tool is a successful, EMPTY wake.
        return validateWakeResponse(transport(job.requestBody), job.motives);
    } catch (const std::exception& e) {
        // Silent to the player: this is stderr, and the turn that would have
        // committed this wake simply has nothing to commit.
        std::fprintf(stderr, "bard: wake failed: %s\n", e.what());
        return std::nullopt;
    } catch (...) {
        std::fprintf(stderr, "bard: wake failed (non-std)\n");
        return std::nullopt;
    }
}

// The worker thread's whole life. ONE wake at a time (REQ-BARD-WAKE-13).
//
// `testTransport` is copied in at construction rather than read from the
// global, so the thread never races a setter. When it is empty the worker
// builds its OWN easy handle — here, inside the thread body, so the handle is
// created and destroyed on the thread that uses it (REQ-BARD-WAKE-16) — and
// binds g_stopping as its abort flag.
void workerMain(HttpTransport testTransport) {
    std::unique_ptr<AiHttpWorkerClient> client;
    HttpTransport transport;
    if (testTransport) {
        transport = std::move(testTransport);  // no client, no curl, no network
    } else {
        client = std::make_unique<AiHttpWorkerClient>(&g_stopping);
        transport = [&client](const std::string& body) {
            // The ORDINARY budget (plan micro-decision 12). Only the overture
            // is the timeout exception, and it runs on the main thread.
            return client->post(body, AiRole::Bard);
        };
    }

    while (true) {
        BardJob job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return g_stopping.load() || g_job.has_value(); });
            // Stop wins over pending work: a wake submitted at shutdown is
            // simply never sent. Nothing depends on it — the bard doing
            // nothing is a supported state on every path in this brick.
            if (g_stopping.load()) break;

            job = std::move(*g_job);
            g_job.reset();
            // g_state is ALREADY Running: bardSubmit set it before it woke us,
            // so a main thread that looks between the submit and here sees the
            // state it must not submit against, never a stale Idle.
        }

        // The mutex is NOT held across the transport call — that is the whole
        // reason the main thread can read the state while this is in flight.
        std::optional<WakeProposal> proposal = runWake(job, transport);

        {
            const std::lock_guard<std::mutex> lock(g_mutex);
            if (proposal) {
                // An empty-but-successful wake still lands here: "the bard
                // declined to act" is a result, and committing it is a no-op.
                g_ready = std::move(proposal);
                g_state = BardState::Ready;
            } else {
                // A rejected response poisons nothing (REQ-BARD-WAKE-23): back
                // to Idle, so the next qualifying event queues a fresh wake.
                g_ready.reset();
                g_state = BardState::Idle;
            }
        }
        g_cv.notify_all();  // wakes any drain waiting out this wake
    }
    // `client` is destroyed HERE, on the worker thread, before this thread ends
    // and therefore before the join in bardStop() returns — so the handle is
    // gone well before aiHttpShutdown() reaches curl_global_cleanup()
    // (REQ-BARD-WAKE-18).
}

// Signal, wake, join. Safe when no thread was ever started, and idempotent.
// MAIN THREAD ONLY. Deliberately takes no lock around the join: the worker
// needs the mutex to finish its current iteration, so holding it here would be
// a guaranteed deadlock.
void stopWorker() {
    if (g_worker.joinable()) {
        g_stopping.store(true);
        g_cv.notify_all();
        g_worker.join();
        g_worker = std::thread();
    }
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_workerRunning = false;

    // REQ-BARD-WAKE-25: a result that became Ready but was never committed is
    // DISCARDED. Done after the join, so the worker cannot be mid-write to
    // these when they are cleared. Nothing is persisted; the next session's
    // first irreversible event queues a fresh wake against a world that has
    // meanwhile moved on, which is a better wake than this stale one.
    g_ready.reset();
    g_job.reset();
    g_state = BardState::Idle;
    g_cv.notify_all();  // release any drain waiting on "not Running"
}

}  // namespace

bool bardEnabled() { return g_enabled; }

void bardRefreshEnabledForTest() { g_enabled = readBardEnv(); }

BardState bardStateNow() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}

bool bardSubmit(BardJob job) {
    if (!g_enabled) return false;  // REQ-BARD-WAKE-20: no wake is ever queued

    {
        const std::lock_guard<std::mutex> lock(g_mutex);

        // BUSY means busy (REQ-BARD-WAKE-13). Ready counts as busy too: a
        // result nobody has committed yet would be overwritten by a second
        // wake, which is a paid-for call thrown away.
        if (g_state != BardState::Idle) {
            g_dirty = true;  // REQ-BARD-WAKE-14: remembered, not queued
            return false;
        }

        // Running is announced BEFORE the worker is woken, so there is no
        // window in which a second caller could see Idle.
        g_state = BardState::Running;
        g_job = std::move(job);
    }
    g_cv.notify_all();  // wake the worker, if there is one
    return true;
}

void bardSetDirty() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_dirty = true;
}

bool bardTakeDirty() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    const bool was = g_dirty;
    g_dirty = false;
    return was;
}

std::optional<WakeProposal> bardTakeReady() {
    std::optional<WakeProposal> taken;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state != BardState::Ready) return std::nullopt;
        taken = std::move(g_ready);
        g_ready.reset();
        g_state = BardState::Idle;
    }
    g_cv.notify_all();  // every state transition is announced
    return taken;
}

void bardStart() {
    // REQ-BARD-WAKE-20: off, or no AI, means NO THREAD is created at all — not
    // a thread that sits idle. bardWorkerRunning() exists so a test can assert
    // that difference, which no amount of reading a log can establish.
    if (!g_enabled || !aiNarrationEnabled()) return;

    if (g_worker.joinable()) return;  // exactly one worker, ever

    g_stopping.store(false);
    HttpTransport testTransport;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        testTransport = g_testTransport;
        g_workerRunning = true;
    }
    g_worker = std::thread(workerMain, std::move(testTransport));
}

void bardStop() { stopWorker(); }

bool bardWorkerRunning() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_workerRunning;
}

void bardSetWorkerTransportForTest(HttpTransport transport) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_testTransport = std::move(transport);
}

void bardResetForTest() {
    stopWorker();  // must precede the lock: the join needs the worker to run
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_testTransport = {};
    g_stopping.store(false);
    // stopWorker already cleared the slot; the flag is this function's own.
    g_dirty = false;
}

void bardWaitForIdleForTest() {
    std::unique_lock<std::mutex> lock(g_mutex);
    ++g_waiting;
    // The counter is incremented BEFORE the wait and while holding the mutex,
    // so a test that spins until it reads 1 knows this thread is committed to
    // the wait rather than merely on its way to it. That is the whole point:
    // it makes "release only once the waiter is provably inside" a fact.
    g_cv.wait(lock, [] { return g_state != BardState::Running; });
    --g_waiting;
}

std::size_t bardWaitingCountForTest() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_waiting;
}
