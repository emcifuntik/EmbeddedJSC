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

    // 6. Class binding: New / methods / Unwrap / Wrap.
    {
        struct Box { int v; };

        auto rtX = ejsc::Runtime{};
        auto cx  = rtX.NewContext();

        auto BoxCls = cx.NewClass<Box>("Box")
            .Constructor([](ejsc::Context&, std::span<const ejsc::Value> args) -> Box* {
                auto* b = new Box;
                if (!args.empty()) b->v = (int)args[0].ToNumber().value_or(0);
                return b;
            })
            .Method("get", [](Box& self, ejsc::Context& c, auto) {
                return ejsc::Value::Number(c, self.v);
            })
            .Method("set", [](Box& self, ejsc::Context& c, auto args) {
                self.v = (int)args[0].ToNumber().value_or(0);
                return ejsc::Value::Undefined(c);
            })
            .Build();

        // JS-side construction.
        cx.SetGlobal("Box", BoxCls.ConstructorValue());
        auto r1 = cx.Eval("const b = new Box(7); b.set(b.get() + 35); b.get()", "cls-1");
        assert(!cx.HasException());
        assert(r1.IsNumber() && r1.ToNumber().value() == 42.0);

        // C++-side New + Unwrap.
        auto v = BoxCls.New({ ejsc::Value::Number(cx, 11.0) });
        assert(BoxCls.IsInstance(v));
        Box* unwrapped = BoxCls.Unwrap(v);
        assert(unwrapped && unwrapped->v == 11);

        // C++-side Wrap on a host-owned object — mutations are visible from JS.
        Box host{ 1 };
        cx.SetGlobal("host", BoxCls.Wrap(&host));
        cx.Eval("host.set(host.get() + 99)", "cls-2");
        assert(!cx.HasException());
        assert(host.v == 100);

        // Cross-class type check: Unwrap on a non-instance returns nullptr.
        auto strVal = ejsc::Value::String(cx, "not a box");
        assert(BoxCls.Unwrap(strVal) == nullptr);

        // Calling a method with the wrong `this` raises an exception.
        cx.Eval("({}).__proto__ = Box.prototype; b.get.call({})", "cls-3");
        assert(cx.HasException());
        (void)cx.TakeException();
    }

    // 7. EvalModule returns a callable namespace for the host (gamedev case).
    //    Caller can hold the namespace, then call exported functions per frame.
    {
        auto rt3 = ejsc::Runtime{};
        auto ctx3 = rt3.NewContext();
        auto ns = ctx3.EvalModule(
            "export function tick() { return 7 }; export const greeting = 'hi'",
            "t6.mjs");
        assert(!ctx3.HasException());
        assert(ns.IsObject());

        auto tick = ns.GetProperty("tick");
        assert(tick.IsFunction());

        auto result = tick.Call(ejsc::Value::Undefined(ctx3), {});
        assert(result.IsNumber());
        assert(result.ToNumber().value() == 7.0);

        auto greeting = ns.GetProperty("greeting");
        assert(greeting.IsString());
        assert(greeting.ToString().value() == "hi");
    }

    std::cout << "test_eval ok\n";
    return 0;
}
