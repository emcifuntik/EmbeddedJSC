#include "ejsc/extra/timer_manager.h"

#include "ejsc/context.h"
#include "ejsc/value.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ejsc::extra {

struct TimerEntry {
    uint32_t id;
    Value callback;
    std::chrono::steady_clock::time_point fireAt;
    std::chrono::milliseconds interval;  // zero == one-shot
    bool cancelled = false;
};

struct TimerManager::State {
    Context* ctx;
    std::vector<TimerEntry> timers;
    uint32_t nextId = 1;
    mutable std::mutex mu;
};

TimerManager::TimerManager(Context& ctx) : m_state(std::make_unique<State>()) {
    m_state->ctx = &ctx;
}

TimerManager::~TimerManager() = default;

namespace {

uint32_t toUint32(const Value& v) {
    if (auto n = v.ToNumber()) return static_cast<uint32_t>(*n);
    return 0;
}

}  // namespace

void TimerManager::Install() {
    auto& ctx = *m_state->ctx;
    State* state = m_state.get();

    auto setTimeoutFn = Value::Function(ctx, "setTimeout",
        [state](Context& c, const Value&, std::span<const Value> args) -> Value {
            if (args.size() < 2 || !args[0].IsFunction()) {
                return Value::Number(c, 0);
            }
            uint32_t delay = toUint32(args[1]);
            std::lock_guard<std::mutex> lock(state->mu);
            uint32_t id = state->nextId++;
            state->timers.push_back(TimerEntry{
                id, args[0],
                std::chrono::steady_clock::now() + std::chrono::milliseconds(delay),
                std::chrono::milliseconds(0),
                false,
            });
            return Value::Number(c, static_cast<double>(id));
        });

    auto setIntervalFn = Value::Function(ctx, "setInterval",
        [state](Context& c, const Value&, std::span<const Value> args) -> Value {
            if (args.size() < 2 || !args[0].IsFunction()) {
                return Value::Number(c, 0);
            }
            uint32_t period = toUint32(args[1]);
            if (period == 0) period = 1;
            std::lock_guard<std::mutex> lock(state->mu);
            uint32_t id = state->nextId++;
            auto now = std::chrono::steady_clock::now();
            state->timers.push_back(TimerEntry{
                id, args[0],
                now + std::chrono::milliseconds(period),
                std::chrono::milliseconds(period),
                false,
            });
            return Value::Number(c, static_cast<double>(id));
        });

    auto clearFn = Value::Function(ctx, "clearTimeout",
        [state](Context& c, const Value&, std::span<const Value> args) -> Value {
            if (args.empty()) return Value::Undefined(c);
            uint32_t id = toUint32(args[0]);
            std::lock_guard<std::mutex> lock(state->mu);
            for (auto& t : state->timers) {
                if (t.id == id) { t.cancelled = true; break; }
            }
            return Value::Undefined(c);
        });

    ctx.SetGlobal("setTimeout",    setTimeoutFn);
    ctx.SetGlobal("setInterval",   setIntervalFn);
    ctx.SetGlobal("clearTimeout",  clearFn);
    ctx.SetGlobal("clearInterval", clearFn);
}

bool TimerManager::Tick() {
    return Tick(std::chrono::steady_clock::now());
}

bool TimerManager::Tick(std::chrono::steady_clock::time_point now) {
    State* state = m_state.get();
    Context& ctx = *state->ctx;

    // Snapshot expired callbacks under the lock; fire them outside the lock so
    // a JS callback re-entering (e.g. installing another timer) doesn't deadlock.
    std::vector<Value> toFire;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        for (auto& t : state->timers) {
            if (t.cancelled) continue;
            if (now < t.fireAt) continue;
            toFire.push_back(t.callback);
            if (t.interval.count() > 0) {
                // Reschedule from current wall time to avoid drift cascade if
                // we ran far behind schedule.
                t.fireAt = now + t.interval;
            } else {
                t.cancelled = true;
            }
        }
    }

    for (const auto& cb : toFire) {
        cb.Call(Value::Undefined(ctx), {});
        ctx.DrainMicrotasks();
    }

    // GC out cancelled/one-shot entries.
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->timers.erase(
            std::remove_if(state->timers.begin(), state->timers.end(),
                [](const TimerEntry& t) { return t.cancelled; }),
            state->timers.end());
        return !state->timers.empty();
    }
}

void TimerManager::Clear() {
    std::lock_guard<std::mutex> lock(m_state->mu);
    m_state->timers.clear();
}

bool TimerManager::Empty() const noexcept {
    std::lock_guard<std::mutex> lock(m_state->mu);
    return m_state->timers.empty();
}

size_t TimerManager::Pending() const noexcept {
    std::lock_guard<std::mutex> lock(m_state->mu);
    return m_state->timers.size();
}

} // namespace ejsc::extra
