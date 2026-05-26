#include <ejsc/ejsc.h>

#include <fstream>
#include <iostream>
#include <sstream>

namespace {
std::string slurp(const char* path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}

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

    // Register a 'math' native module — pure C++, no JS bootstrap.
    auto math = ctx.NewModule("math");
    math.ExportFunction("add", [](ejsc::Context& c, const ejsc::Value&,
                                  std::span<const ejsc::Value> args) {
        double a = args.size() > 0 ? args[0].ToNumber().value_or(0.0) : 0.0;
        double b = args.size() > 1 ? args[1].ToNumber().value_or(0.0) : 0.0;
        return ejsc::Value::Number(c, a + b);
    });
    math.ExportFunction("sub", [](ejsc::Context& c, const ejsc::Value&,
                                  std::span<const ejsc::Value> args) {
        double a = args.size() > 0 ? args[0].ToNumber().value_or(0.0) : 0.0;
        double b = args.size() > 1 ? args[1].ToNumber().value_or(0.0) : 0.0;
        return ejsc::Value::Number(c, a - b);
    });
    math.Build();

    std::string src = slurp("test.js");
    if (src.empty()) {
        std::cerr << "could not read test.js (cwd issue?)\n";
        return 1;
    }

    ctx.EvalModule(src, "test.js");
    if (ctx.HasException()) {
        auto exc = ctx.TakeException().ToString().value_or("<unknown>");
        std::cerr << "exception: " << exc << '\n';
        return 1;
    }
    return 0;
}
