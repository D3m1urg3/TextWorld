// The spinner (see spinner.hpp for the contract).
#include "spinner.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace {

// ASCII only (REQ-UI-29). No braille, no block characters: those are the
// widths a terminal is allowed to disagree about, and a spinner that leaves a
// half-erased column behind is exactly what REQ-POLISH-27 forbids.
constexpr const char* kFrames = "|/-\\";
constexpr int kFrameCount = 4;
constexpr int kFrameIntervalMs = 100;

std::atomic<int64_t> g_threadsStarted{0};

// Guarded because the spinner THREAD reads it while the main thread may be
// setting it between turns.
std::mutex g_sinkMutex;
std::function<void(std::string_view)> g_sink;

void emit(std::string_view text) {
    std::function<void(std::string_view)> sink;
    {
        const std::lock_guard<std::mutex> lock(g_sinkMutex);
        sink = g_sink;
    }
    if (sink) {
        sink(text);
        return;
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

}  // namespace

// Declared in spinner.hpp, defined here so the header pulls in no <thread>.
struct SpinnerState {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    bool drew = false;
};

Spinner::Spinner(TermStyle style, bool aiEnabled) {
    // REQ-POLISH-26 and REQ-POLISH-28. Both gates are checked BEFORE the thread
    // is created, so a suppressed spinner costs a branch and nothing else.
    if (!style.attrs || !aiEnabled) return;

    state_ = std::make_unique<SpinnerState>();
    g_threadsStarted.fetch_add(1);
    SpinnerState* state = state_.get();
    state_->thread = std::thread([state] {
        int frame = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                // The wait is INTERRUPTIBLE, so the destructor does not have to
                // sit through a whole frame interval before the thread notices
                // it should stop — a turn that returns in 20 ms must not be
                // delayed 100 ms by its own spinner.
                if (state->cv.wait_for(lock,
                                       std::chrono::milliseconds(kFrameIntervalMs),
                                       [state] { return state->stop; })) {
                    return;
                }
                state->drew = true;
            }
            const char f = kFrames[frame % kFrameCount];
            ++frame;
            emit(std::string("\r") + f);
        }
    });
}

Spinner::~Spinner() {
    if (state_ == nullptr) return;

    SpinnerState* state = state_.get();
    {
        const std::lock_guard<std::mutex> lock(state->mutex);
        state->stop = true;
    }
    state->cv.notify_all();
    if (state->thread.joinable()) state->thread.join();

    // AFTER the join, so nothing can be written between the last frame and this
    // clear. REQ-POLISH-27: carriage return, a space over the one column a frame
    // occupies, carriage return again — the line is left exactly as it was found,
    // on the success path, the failure path and the timeout path alike.
    if (state->drew) emit("\r \r");
}

int64_t Spinner::threadsStarted() { return g_threadsStarted.load(); }

void Spinner::setSink(std::function<void(std::string_view)> sink) {
    const std::lock_guard<std::mutex> lock(g_sinkMutex);
    g_sink = std::move(sink);
}

int Spinner::frameIntervalMs() { return kFrameIntervalMs; }
