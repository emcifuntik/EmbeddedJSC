// Class binding demo. We expose two flavours:
//
//   1. `new Vec3(x, y, z)` — JS-owned: the finalizer deletes the C++ Vec3
//      when the JS handle is GC'd.
//   2. `getPlayerPosition()` returns a borrowed Vec3 wrapping a static
//      host-owned object. JS can read/mutate it; the host's lifetime rules.

#include <ejsc/ejsc.h>

#include <cmath>
#include <iostream>
#include <span>
#include <string>

struct Vec3 {
    double x{}, y{}, z{};

    double length() const { return std::sqrt(x * x + y * y + z * z); }
    std::string str() const {
        return "Vec3(" + std::to_string(x) + ", " + std::to_string(y) + ", " +
               std::to_string(z) + ")";
    }
};

static Vec3 g_playerPos{ 100.0, 200.0, 50.0 };

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

    auto Vec3Cls = ctx.NewClass<Vec3>("Vec3")
        .Constructor([](ejsc::Context&, std::span<const ejsc::Value> args) -> Vec3* {
            auto* v = new Vec3;
            if (args.size() > 0) v->x = args[0].ToNumber().value_or(0.0);
            if (args.size() > 1) v->y = args[1].ToNumber().value_or(0.0);
            if (args.size() > 2) v->z = args[2].ToNumber().value_or(0.0);
            return v;
        })
        .Method("length", [](Vec3& self, ejsc::Context& c, auto) {
            return ejsc::Value::Number(c, self.length());
        })
        .Method("toString", [](Vec3& self, ejsc::Context& c, auto) {
            return ejsc::Value::String(c, self.str());
        })
        .Method("x", [](Vec3& self, ejsc::Context& c, auto) {
            return ejsc::Value::Number(c, self.x);
        })
        .Method("y", [](Vec3& self, ejsc::Context& c, auto) {
            return ejsc::Value::Number(c, self.y);
        })
        .Method("z", [](Vec3& self, ejsc::Context& c, auto) {
            return ejsc::Value::Number(c, self.z);
        })
        .Build();

    // Install as a global so JS can `new Vec3(...)`.
    ctx.SetGlobal("Vec3", Vec3Cls.ConstructorValue());

    // Host function exposing the borrowed pointer pattern.
    ctx.SetGlobal("getPlayerPosition", ejsc::Value::Function(ctx, "getPlayerPosition",
        [Vec3Cls](ejsc::Context& c, const ejsc::Value&, std::span<const ejsc::Value>) {
            return Vec3Cls.Wrap(&g_playerPos);
        }));

    // Host function that uses Unwrap to read back from a JS-passed instance.
    ctx.SetGlobal("describe", ejsc::Value::Function(ctx, "describe",
        [Vec3Cls](ejsc::Context& c, const ejsc::Value&, std::span<const ejsc::Value> args) {
            if (args.empty()) return ejsc::Value::String(c, "describe: no arg");
            Vec3* v = Vec3Cls.Unwrap(args[0]);
            if (!v) return ejsc::Value::String(c, "describe: not a Vec3");
            return ejsc::Value::String(c, "describe: " + v->str());
        }));

    ctx.Eval(R"(
        const v = new Vec3(1, 2, 2);
        print('JS-owned Vec3:', v.toString(), 'length =', v.length());

        const p = getPlayerPosition();
        print('Borrowed player pos:', p.toString());

        // Round-trip an instance through C++ via Unwrap.
        print(describe(v));
        print(describe(p));
        print(describe('not-a-vec3'));
    )", "classes-demo.js");

    if (ctx.HasException()) {
        std::cerr << "exception: " << ctx.TakeException().ToString().value_or("?") << '\n';
        return 1;
    }

    // The borrowed pointer is still ours — mutate it, JS sees the change.
    g_playerPos.x = 999.0;
    ctx.Eval("print('After mutation, player pos:', getPlayerPosition().toString());", "main-mutate.js");

    return 0;
}
