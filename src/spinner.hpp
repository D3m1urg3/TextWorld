// The spinner shown while a turn is blocked on a network call (REQ-POLISH-25..
// -28). An RAII type: it starts a thread on construction and erases itself on
// destruction, so the erase is reached on the success path, the failure path and
// the timeout path alike — which is what REQ-POLISH-27 needs.
//
// It writes to stdout from a SECOND thread while the main thread is inside a
// blocking libcurl call. That is safe here because of where it is scoped: the
// only stdout writer during its lifetime is the spinner itself. runTurn composes
// a string and returns it; main() prints that string AFTER runTurn has returned
// and therefore after this destructor has run.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include "term.hpp"

// The thread, its stop flag and its condition variable. Defined in the .cpp so
// this header pulls in no <thread>, and held by unique_ptr so a Spinner owns its
// own state rather than sharing a file-static one — two of them at once must not
// interfere, even though the binary only ever constructs one at a time.
struct SpinnerState;

class Spinner {
public:
    // Starts a thread only when BOTH gates allow it:
    //   - style.attrs (REQ-POLISH-26) — already false for a pipe and for
    //     TERM=dumb, so no escape byte reaches a non-terminal and the golden
    //     comparisons in tests.cpp are untouched;
    //   - aiEnabled (REQ-POLISH-28) — template mode makes no network call, so
    //     there is nothing to wait for and no thread is started at all.
    Spinner(TermStyle style, bool aiEnabled);

    // Signals the thread, joins it, and erases whatever it drew. Erasure happens
    // AFTER the join, so nothing can be written between the last frame and the
    // clear.
    ~Spinner();

    Spinner(const Spinner&) = delete;
    Spinner& operator=(const Spinner&) = delete;

    // How many spinner threads this process has started. The offline gate asserts
    // this does not move under TEXTWORLD_AI=0 (REQ-POLISH-28) — a counter is
    // what makes "no thread starts" checkable rather than merely likely.
    static int64_t threadsStarted();

    // Redirect the frames somewhere a test can read. Set from the MAIN thread
    // before any Spinner exists; an empty sink restores stdout. Same shape and
    // purpose as profileSetSink().
    static void setSink(std::function<void(std::string_view)> sink);

    // The frame interval, exposed so a test can sleep past several of them
    // rather than hardcoding a number that could drift.
    static int frameIntervalMs();

private:
    // Null when either gate suppressed the spinner — which is the common case
    // offline, and is what makes "no thread was started" a fact about the
    // object rather than about a global.
    std::unique_ptr<SpinnerState> state_;
};
