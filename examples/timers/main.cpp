#include <ejsc/ejsc.h>
#include <ejsc/extra/timer_manager.h>

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    ejsc::Runtime rt;
    auto ctx = rt.NewContext();

    ctx.SetGlobal("print", ejsc::Value::Function(ctx, "print",
        [](ejsc::Context& c, const ejsc::Value&, std::span<const ejsc::Value> args) {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i > 0) std::cout << ' ';
                if (auto s = args[i].ToString()) std::cout << *s;
            }
            std::cout << '\n';
            return ejsc::Value::Undefined(c);
        }));

    ejsc::extra::TimerManager timers(ctx);
    timers.Install();

    // Two one-shots and one repeating interval. The interval cancels itself
    // after three ticks.
    ctx.Eval(
        R"(
        print('init');
        setTimeout(() => print('one-shot 50ms'), 50);
        setTimeout(() => print('one-shot 200ms'), 200);
        let n = 0;
        const id = setInterval(() => {
            print('interval', ++n);
            if (n >= 3) clearInterval(id);
        }, 80);
        )",
        "timers-demo.js");

    if (ctx.HasException()) {
        std::cerr << "exception: " << ctx.TakeException().ToString().value_or("?") << '\n';
        return 1;
    }

    // Drive the timer loop. In a game this is your per-frame Tick().
    auto start = std::chrono::steady_clock::now();
    while (timers.Tick()) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            std::cerr << "timeout — bailing out\n";
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::cout << "done\n";
    return 0;
}
