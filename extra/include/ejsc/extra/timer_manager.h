#pragma once

// TimerManager — opt-in reference implementation of setTimeout / setInterval
// / clearTimeout / clearInterval for an ejsc::Context.
//
// Why this is not in libejsc: timers depend on the host's event loop. A game
// engine ticks every frame, a worker thread drives its own poll loop, a CLI
// might just sleep. The library cannot pick one for you. This helper is a
// good starting point — copy it, fork it, or build on top of it.
//
// Usage:
//
//     ejsc::Runtime rt;
//     auto ctx = rt.NewContext();
//     ejsc::extra::TimerManager timers(ctx);
//     timers.Install();
//
//     // ... evaluate JS that uses setTimeout/setInterval ...
//
//     // Per frame, or in your event loop:
//     while (timers.Tick()) { /* yield, sleep, etc. */ }
//
// Timer callbacks are invoked with zero arguments. (`setTimeout(fn, ms, ...args)`
// vararg form is not implemented; extend if you need it.)
//
// The TimerManager must not outlive its Context.

#include <chrono>
#include <cstddef>
#include <memory>

namespace ejsc {
class Context;
}

namespace ejsc::extra {

class TimerManager {
public:
    explicit TimerManager(Context& ctx);
    ~TimerManager();

    TimerManager(const TimerManager&) = delete;
    TimerManager& operator=(const TimerManager&) = delete;

    // Register setTimeout / setInterval / clearTimeout / clearInterval as
    // globals on the bound Context. Call once after construction.
    void Install();

    // Fire any timers whose deadline has passed at `now`. Reschedules intervals.
    // Drains microtasks after firing each callback so promise continuations
    // chained inside the callback run before the host returns to its main loop.
    // Returns true while pending timers remain.
    bool Tick(std::chrono::steady_clock::time_point now);
    bool Tick();   // uses steady_clock::now()

    // Cancel all timers (host shutdown, mod reload, etc.).
    void Clear();

    bool   Empty()   const noexcept;
    size_t Pending() const noexcept;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace ejsc::extra
