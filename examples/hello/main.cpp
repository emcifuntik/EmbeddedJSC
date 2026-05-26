#include <ejsc/ejsc.h>

#include <iostream>

int main() {
    ejsc::Runtime rt;
    auto ctx = rt.NewContext();

    // Expose a print() global wired to std::cout.
    ctx.SetGlobal("print", ejsc::Value::Function(ctx, "print",
        [](ejsc::Context& c, const ejsc::Value&, std::span<const ejsc::Value> args) {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i > 0) std::cout << ' ';
                if (auto s = args[i].ToString()) std::cout << *s;
            }
            std::cout << '\n';
            return ejsc::Value::Undefined(c);
        }));

    ejsc::Value result = ctx.Eval("print('hello, ejsc'); 1 + 2", "hello.js");
    if (ctx.HasException()) {
        auto exc = ctx.TakeException().ToString().value_or("<unknown>");
        std::cerr << "exception: " << exc << '\n';
        return 1;
    }
    if (auto n = result.ToNumber()) {
        std::cout << "result = " << *n << '\n';
    }
    return 0;
}
