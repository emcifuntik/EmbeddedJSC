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

    std::cout << "test_eval ok\n";
    return 0;
}
