#include <ejsc/ejsc.h>

#include <cassert>
#include <iostream>

int main() {
    ejsc::Runtime rt;
    auto ctx = rt.NewContext();

    // 1. Number arithmetic
    {
        auto v = ctx.Eval("2 + 3", "t1");
        assert(v.IsNumber());
        assert(v.ToNumber().value() == 5.0);
    }

    // 2. String round-trip
    {
        auto v = ctx.Eval("'foo' + 'bar'", "t2");
        assert(v.IsString());
        assert(v.ToString().value() == "foobar");
    }

    // 3. Native function callable from JS
    {
        ctx.SetGlobal("triple", ejsc::Value::Function(ctx, "triple",
            [](ejsc::Context& c, const ejsc::Value&, std::span<const ejsc::Value> args) {
                double x = args.size() ? args[0].ToNumber().value_or(0.0) : 0.0;
                return ejsc::Value::Number(c, x * 3.0);
            }));
        auto v = ctx.Eval("triple(7)", "t3");
        assert(v.IsNumber());
        assert(v.ToNumber().value() == 21.0);
    }

    // 4. Exception is captured
    {
        ctx.Eval("throw new Error('boom')", "t4");
        assert(ctx.HasException());
        auto exc = ctx.TakeException();
        assert(exc.ToString().value().find("boom") != std::string::npos);
        assert(!ctx.HasException());
    }

    // 5. Native synthetic module: import { add } from 'math'
    {
        auto rt2 = ejsc::Runtime{};
        auto ctx2 = rt2.NewContext();

        // capture flag by reference via a global; simpler: stash the result on a global.
        ctx2.SetGlobal("__capture", ejsc::Value::Number(ctx2, 0));
        ctx2.SetGlobal("setCapture", ejsc::Value::Function(ctx2, "setCapture",
            [](ejsc::Context& c, const ejsc::Value&, std::span<const ejsc::Value> args) {
                c.SetGlobal("__capture", args.size() ? args[0] : ejsc::Value::Undefined(c));
                return ejsc::Value::Undefined(c);
            }));

        auto m = ctx2.NewModule("math");
        m.ExportFunction("add", [](ejsc::Context& c, const ejsc::Value&,
                                   std::span<const ejsc::Value> args) {
            double a = args[0].ToNumber().value_or(0.0);
            double b = args[1].ToNumber().value_or(0.0);
            return ejsc::Value::Number(c, a + b);
        });
        m.Build();

        ctx2.EvalModule("import { add } from 'math'; setCapture(add(40, 2))", "t5.mjs");
        assert(!ctx2.HasException());
        auto cap = ctx2.GetGlobal("__capture");
        assert(cap.IsNumber());
        assert(cap.ToNumber().value() == 42.0);
    }

    std::cout << "test_eval ok\n";
    return 0;
}
