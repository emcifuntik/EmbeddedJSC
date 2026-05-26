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

        // instanceof works on the constructor function.
        auto isInst = cx.Eval("b instanceof Box", "cls-4");
        assert(isInst.IsBool() && isInst.ToBool().value() == true);
    }

    // 6b. Accessor properties (read-write + read-only) and inheritance.
    {
        struct Base { int v = 0; };
        struct Derived : Base { int extra = 99; };

        auto rtY = ejsc::Runtime{};
        auto cy  = rtY.NewContext();

        auto BaseCls = cy.NewClass<Base>("Base")
            .Constructor([](ejsc::Context&, std::span<const ejsc::Value>) -> Base* { return new Base; })
            .Property("v",
                [](const Base& s, ejsc::Context& c) { return ejsc::Value::Number(c, s.v); },
                [](Base& s, ejsc::Context&, const ejsc::Value& val) {
                    s.v = (int)val.ToNumber().value_or(0);
                })
            .Property("doubled",  // read-only computed
                [](const Base& s, ejsc::Context& c) { return ejsc::Value::Number(c, s.v * 2); })
            .Method("describe", [](Base& s, ejsc::Context& c, auto) {
                return ejsc::Value::String(c, "base(" + std::to_string(s.v) + ")");
            })
            .Build();

        auto DerivedCls = cy.NewClass<Derived>("Derived")
            .Extends(BaseCls)
            .Constructor([](ejsc::Context&, std::span<const ejsc::Value>) -> Derived* { return new Derived; })
            .Property("extra",
                [](const Derived& s, ejsc::Context& c) { return ejsc::Value::Number(c, s.extra); },
                [](Derived& s, ejsc::Context&, const ejsc::Value& val) {
                    s.extra = (int)val.ToNumber().value_or(0);
                })
            .Build();

        cy.SetGlobal("Base", BaseCls.ConstructorValue());
        cy.SetGlobal("Derived", DerivedCls.ConstructorValue());

        // Write to inherited accessor.
        auto r1 = cy.Eval("const d = new Derived(); d.v = 10; d.v + d.doubled", "acc-1");
        assert(!cy.HasException());
        assert(r1.IsNumber() && r1.ToNumber().value() == 30.0);   // 10 + 10*2

        // Read-only property setter throws.
        cy.Eval("(new Base()).doubled = 5", "acc-2");
        assert(cy.HasException());
        auto exc = cy.TakeException().ToString().value_or("");
        assert(exc.find("read-only") != std::string::npos);

        // Own + inherited methods both callable on a derived instance.
        auto r3 = cy.Eval("const d2 = new Derived(); d2.v = 7; d2.describe()", "acc-3");
        assert(!cy.HasException());
        assert(r3.IsString() && r3.ToString().value() == "base(7)");

        // instanceof walks the chain.
        auto r4 = cy.Eval("const d3 = new Derived(); [d3 instanceof Derived, d3 instanceof Base]", "acc-4");
        assert(!cy.HasException());
        assert(r4.IsObject());
        assert(r4.GetProperty("0").ToBool().value() == true);
        assert(r4.GetProperty("1").ToBool().value() == true);

        // Cross-class Unwrap: a Derived value yields a valid Base* AND a valid Derived*.
        auto dVal = cy.Eval("const d4 = new Derived(); d4.v = 11; d4.extra = 22; d4", "acc-5");
        assert(!cy.HasException());
        Base*    asBase    = BaseCls.Unwrap(dVal);
        Derived* asDerived = DerivedCls.Unwrap(dVal);
        assert(asBase && asBase->v == 11);
        assert(asDerived && asDerived->v == 11 && asDerived->extra == 22);

        // Reverse direction fails.
        auto bVal = cy.Eval("new Base()", "acc-6");
        assert(DerivedCls.Unwrap(bVal) == nullptr);
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
