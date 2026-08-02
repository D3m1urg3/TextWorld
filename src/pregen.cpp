// See pregen.hpp for the contract, and for the two grep checks that keep this
// file free of database access. Nothing in here takes a Db, and no SQL of any
// kind appears below — the job arrives pre-snapshotted from the main thread.
#include "pregen.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "aihttp.hpp"  // makeAnthropicTransport + AiHttpWorkerClient

namespace {

// TEXTWORLD_PREGEN: ON unless set to exactly "0" (REQ-PREGEN-1). Deliberately
// the TEXTWORLD_AI rule and not TEXTWORLD_PROFILE's — an unset variable means
// the feature is on, so a player who has never heard of it gets the fast turn.
bool readPregenEnv() {
    const char* v = std::getenv("TEXTWORLD_PREGEN");
    if (v == nullptr) return true;
    return std::string(v) != "0";
}

// Cached at static-init time: the getenv happens once per process, so the turn
// path pays a bool read.
bool g_enabled = readPregenEnv();

// --- the candidate store ----------------------------------------------------
//
// One mutex guards BOTH the slot map and the pending queue: they are mutated
// together on every state transition, and a second lock would buy nothing but
// an ordering rule to get wrong. It is NEVER held across a transport call —
// every path that makes one drops the lock first and re-takes it after.

using Key = std::pair<int64_t, std::string>;

struct Slot {
    PregenState state = PregenState::Absent;
    std::optional<RoomProposal> proposal;
    int64_t snapshotTurn = 0;
};

std::mutex g_mutex;
std::map<Key, Slot> g_slots;

// Signalled after EVERY slot transition and on every submit, so both the
// worker (waiting for work) and the tick (waiting out a Running job) wake on
// the same condition variable. One condvar for one mutex.
std::condition_variable g_cv;

// The worker's stop flag. Also handed to the worker's AiHttpWorkerClient as
// its abort flag, so setting it tears down an in-flight transfer through
// libcurl's progress callback rather than waiting out the 8 s timeout
// (REQ-PREGEN-20). Atomic because libcurl polls it from inside perform().
std::atomic<bool> g_stopping{false};

// Pending work, front to back. UNKEYED on purpose: the tick's Queued path
// linear-scans it for its own key and erases that element. Depths are
// single-digit — one to three latent exits per room — so a scan is the right
// structure, and an index would be more machinery than the data justifies.
std::deque<PregenJob> g_pending;

// The worker itself. `g_worker` and `g_workerRunning` are touched ONLY by the
// main thread (pregenStart / pregenStop / pregenResetForTest), which is what
// makes their lifetime reasoning a straight line rather than a protocol.
// `g_workerRunning` is still read under the mutex because the test hook may
// ask from anywhere.
std::thread g_worker;
bool g_workerRunning = false;

// TEST-ONLY. When set, the worker uses this INSTEAD of constructing an
// AiHttpWorkerClient — so the concurrency tests touch no libcurl at all.
HttpTransport g_testTransport;

// How many threads are inside the Running wait below. Guarded by g_mutex.
// Test observability only (see pregenWaitingCountForTest); nothing in the
// production path branches on it.
std::size_t g_waiting = 0;

Key keyOf(int64_t room, const std::string& direction) {
    return Key{room, direction};
}

// The whole worker body, and deliberately synchronous so it is testable
// WITHOUT a thread. The work itself is architectProposeRoom — the SAME code
// the synchronous path runs for Phase 1's AI half — fed the job's snapshot
// instead of live reads. No database, no retries (REQ-PREGEN-7, -9).
//
// What is added here is the catch, including the catch-all: a throwing
// transport must never escape onto the worker thread, where there is no caller
// to catch it and the process would die. The diagnostic says `pregen` rather
// than `architectGenerate` because a failed background job and a walled turn
// are different events to whoever is reading stderr.
std::optional<RoomProposal> runJob(const PregenJob& job,
                                   const HttpTransport& transport) {
    try {
        return architectProposeRoom(job.contextPayload, job.enemyBlurbs,
                                    job.direction, transport);
    } catch (const std::exception& e) {
        // Silent to the player (REQ-PREGEN-9): this is stderr, and the turn
        // that eventually walks this exit simply takes the miss path.
        std::fprintf(stderr, "pregen: job failed: %s\n", e.what());
        return std::nullopt;
    } catch (...) {
        std::fprintf(stderr, "pregen: job failed (non-std)\n");
        return std::nullopt;
    }
}

// Remove the pending job for `key` and hand it back, or nullopt if there is
// none. Caller holds g_mutex.
std::optional<PregenJob> takePendingLocked(const Key& key) {
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
        if (it->room == key.first && it->direction == key.second) {
            PregenJob job = std::move(*it);
            g_pending.erase(it);
            return job;
        }
    }
    return std::nullopt;
}

// Fold a finished job's result into its slot. Caller holds g_mutex.
// A success stores the candidate; a FAILURE resets the slot to Absent rather
// than poisoning it (REQ-PREGEN-10) — the key becomes eligible again the next
// time the player enters the room, so retries are bounded by room entries
// rather than by a timer.
void storeResultLocked(const Key& key, const PregenJob& job,
                       const std::optional<RoomProposal>& proposal) {
    Slot& slot = g_slots[key];
    if (proposal) {
        slot.state = PregenState::Ready;
        slot.proposal = *proposal;
        slot.snapshotTurn = job.snapshotTurn;
    } else {
        slot = Slot{};  // back to Absent, wholesale — Slot's own defaults ARE
                        // the cleared state, so there is nothing to restate
    }
}

// The worker thread's whole life. ONE job at a time, drained from the FRONT of
// the queue (REQ-PREGEN-6).
//
// `testTransport` is copied in at construction rather than read from the
// global, so the thread never races a setter. When it is empty the worker
// builds its OWN easy handle — here, inside the thread body, so the handle is
// created and destroyed on the thread that uses it (REQ-PREGEN-8) — and binds
// g_stopping as its abort flag.
void workerMain(HttpTransport testTransport) {
    std::unique_ptr<AiHttpWorkerClient> client;
    HttpTransport transport;
    if (testTransport) {
        transport = std::move(testTransport);  // no client, no curl, no network
    } else {
        client = std::make_unique<AiHttpWorkerClient>(&g_stopping);
        transport = [&client](const std::string& body) {
            return client->post(body, AiRole::Generate);
        };
    }

    while (true) {
        PregenJob job;
        Key key;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] {
                return g_stopping.load() || !g_pending.empty();
            });
            // Stop wins over pending work: a queued job at shutdown is simply
            // never run. Nothing depends on it — the tick's miss path is the
            // fallback for every key that has no candidate.
            if (g_stopping.load()) break;

            job = std::move(g_pending.front());
            g_pending.pop_front();
            key = keyOf(job.room, job.direction);
            g_slots[key].state = PregenState::Running;
        }
        // Announce Running before the call, so a tick that arrives mid-flight
        // sees the state it must wait on rather than a stale Queued.
        g_cv.notify_all();

        // The mutex is NOT held across the transport call — that is the whole
        // reason the tick can read the store while this is in flight.
        const std::optional<RoomProposal> proposal = runJob(job, transport);

        {
            const std::lock_guard<std::mutex> lock(g_mutex);
            storeResultLocked(key, job, proposal);
        }
        g_cv.notify_all();  // wakes any tick waiting on this key
    }
    // `client` is destroyed HERE, on the worker thread, before this thread
    // ends and therefore before the join in pregenStop() returns — so the
    // handle is gone well before aiHttpShutdown() reaches
    // curl_global_cleanup() (REQ-PREGEN-19).
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
}

}  // namespace

const char* pregenOutcomeName(PregenOutcome outcome) {
    switch (outcome) {
        case PregenOutcome::Hit:
            return "hit";
        case PregenOutcome::Waited:
            return "waited";
        case PregenOutcome::RanQueued:
            return "ran_queued";
        case PregenOutcome::Miss:
            return "miss";
    }
    return "miss";
}

bool pregenEnabled() { return g_enabled; }

void pregenRefreshEnabledForTest() { g_enabled = readPregenEnv(); }

void pregenSubmit(PregenJob job) {
    if (!g_enabled) return;  // REQ-PREGEN-1: no job is ever queued

    const Key key = keyOf(job.room, job.direction);
    const std::lock_guard<std::mutex> lock(g_mutex);

    // One candidate per key, ever (REQ-PREGEN-11, REQ-PREGEN-21). The
    // scheduler already checks this, but enforcing it here too means no future
    // caller can queue a room twice — which would be a second paid-for call
    // for the same room.
    Slot& slot = g_slots[key];
    if (slot.state != PregenState::Absent) return;

    slot.state = PregenState::Queued;
    g_pending.push_back(std::move(job));
    g_cv.notify_all();  // wake the worker, if there is one
}

void pregenStart() {
    // REQ-PREGEN-1 / REQ-PREGEN-2: off, or no architect, means NO THREAD is
    // created at all — not a thread that sits idle. pregenWorkerRunning()
    // exists so a test can assert that difference, which no amount of reading
    // the profile log can establish.
    if (!g_enabled || !architectEnabled()) return;

    if (g_worker.joinable()) return;  // exactly one worker, ever (REQ-PREGEN-6)

    g_stopping.store(false);
    HttpTransport testTransport;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        testTransport = g_testTransport;
        g_workerRunning = true;
    }
    g_worker = std::thread(workerMain, std::move(testTransport));
}

bool pregenWorkerRunning() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_workerRunning;
}

void pregenSetWorkerTransportForTest(HttpTransport transport) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_testTransport = std::move(transport);
}

PregenState pregenStateOf(int64_t room, const std::string& direction) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_slots.find(keyOf(room, direction));
    return it == g_slots.end() ? PregenState::Absent : it->second.state;
}

PregenResult pregenAcquire(int64_t room, const std::string& direction,
                           const HttpTransport* transport) {
    PregenResult result;  // Miss, with no proposal, is the default everywhere

    // REQ-PREGEN-1: off means the tick behaves byte-for-byte as it did before
    // this file existed. Nothing is read, nothing is locked.
    if (!g_enabled) return result;

    const Key key = keyOf(room, direction);
    std::unique_lock<std::mutex> lock(g_mutex);
    const auto it = g_slots.find(key);
    if (it == g_slots.end()) return result;  // Absent -> Miss

    switch (it->second.state) {
        case PregenState::Absent:
            return result;  // Miss

        case PregenState::Ready: {
            // A candidate was waiting: the turn makes NO network call at all.
            // The proposal is COPIED, not moved out: REQ-PREGEN-11 says a
            // candidate is never evicted for the life of the process, and a
            // slot whose optional had been emptied would be a Ready state with
            // nothing in it — a shape no reader should have to reason about.
            // The copy is a few hundred bytes, once per room, on a path that
            // is about to write to the database anyway.
            result.outcome = PregenOutcome::Hit;
            result.proposal = it->second.proposal;
            result.snapshotTurn = it->second.snapshotTurn;
            return result;
        }

        case PregenState::Queued: {
            // The worker has not started this job. Take it OUT of the queue
            // and run it here rather than waiting for the worker to reach it:
            // with one serial worker, waiting would bound the tick at
            // (queue depth x 8 s) (REQ-PREGEN-16). Same snapshot, so no
            // duplicate call is ever made for this key.
            std::optional<PregenJob> job = takePendingLocked(key);
            if (!job) {
                // The queue and the slot map are mutated together under this
                // mutex, so Queued always has a pending job. Reaching here
                // means that invariant broke; degrade to a miss rather than
                // inventing a recovery for a state that cannot occur.
                return result;
            }
            it->second.state = PregenState::Running;

            // The mutex is NEVER held across a transport call.
            lock.unlock();
            const HttpTransport effective =
                transport != nullptr ? *transport
                                     : makeAnthropicTransport(AiRole::Generate);
            const auto start = std::chrono::steady_clock::now();
            const std::optional<RoomProposal> proposal = runJob(*job, effective);
            const std::chrono::duration<double, std::milli> elapsed =
                std::chrono::steady_clock::now() - start;
            lock.lock();

            storeResultLocked(key, *job, proposal);
            result.outcome = PregenOutcome::RanQueued;
            result.proposal = proposal;
            result.runMs = elapsed.count();
            lock.unlock();
            g_cv.notify_all();  // every slot transition is announced
            return result;
        }

        case PregenState::Running: {
            // The worker is mid-call for THIS key. Wait it out rather than
            // starting a second call: one room is never paid for twice
            // (REQ-PREGEN-16). The wait is bounded by the transport's own 8 s
            // timeout, because the thing being waited on is one perform().
            //
            // Note what is NOT here: a wait for the QUEUE to drain. A tick
            // never waits behind an unrelated job — that case took the Queued
            // branch above and ran its own job on this thread.
            const auto start = std::chrono::steady_clock::now();
            ++g_waiting;
            // Re-looked up on each wake rather than captured: the mutex is
            // released while waiting, and re-finding by key costs a map probe
            // and owes nothing to iterator-stability reasoning.
            g_cv.wait(lock, [&key] {
                const auto slot = g_slots.find(key);
                return slot == g_slots.end() ||
                       slot->second.state != PregenState::Running;
            });
            --g_waiting;
            const std::chrono::duration<double, std::milli> elapsed =
                std::chrono::steady_clock::now() - start;

            result.outcome = PregenOutcome::Waited;
            result.waitMs = elapsed.count();
            // Whatever the worker landed on. A job that FAILED left the slot
            // Absent, so there is no proposal and the tick falls back to the
            // synchronous path — a waited miss, reported honestly as `waited`
            // because the tick really did wait.
            const auto settled = g_slots.find(key);
            if (settled != g_slots.end() &&
                settled->second.state == PregenState::Ready) {
                result.proposal = settled->second.proposal;
                result.snapshotTurn = settled->second.snapshotTurn;
            }
            return result;
        }
    }
    return result;
}

void pregenStop() { stopWorker(); }

void pregenResetForTest() {
    stopWorker();  // must precede the lock: the join needs the worker to run
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_slots.clear();
    g_pending.clear();
    g_testTransport = {};
    g_stopping.store(false);
}

void pregenInjectReadyForTest(int64_t room, const std::string& direction,
                              const RoomProposal& proposal,
                              int64_t snapshotTurn) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    Slot& slot = g_slots[keyOf(room, direction)];
    slot.state = PregenState::Ready;
    slot.proposal = proposal;
    slot.snapshotTurn = snapshotTurn;
}

std::size_t pregenPendingCountForTest() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_pending.size();
}

std::size_t pregenWaitingCountForTest() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_waiting;
}
