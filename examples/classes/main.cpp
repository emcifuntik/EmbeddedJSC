// Class binding demo:
//   - Vec3 with accessor properties (v.x, v.y, v.z).
//   - Entity / Player single inheritance, where Player inherits Entity's
//     properties and adds its own + a method.

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

struct Entity {
    std::string name = "<unnamed>";
    double x{}, y{}, z{};
    virtual ~Entity() = default;
    std::string describe() const {
        return name + "@(" + std::to_string(x) + "," + std::to_string(y) + "," +
               std::to_string(z) + ")";
    }
};

struct Player : Entity {
    int health = 100;
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

    // -----------------------------------------------------------------------
    // Vec3 with accessor properties
    // -----------------------------------------------------------------------

    auto Vec3Cls = ctx.NewClass<Vec3>("Vec3")
        .Constructor([](ejsc::Context&, std::span<const ejsc::Value> args) -> Vec3* {
            auto* v = new Vec3;
            if (args.size() > 0) v->x = args[0].ToNumber().value_or(0.0);
            if (args.size() > 1) v->y = args[1].ToNumber().value_or(0.0);
            if (args.size() > 2) v->z = args[2].ToNumber().value_or(0.0);
            return v;
        })
        .Property("x",
            [](const Vec3& s, ejsc::Context& c) { return ejsc::Value::Number(c, s.x); },
            [](Vec3& s, ejsc::Context&, const ejsc::Value& v) { s.x = v.ToNumber().value_or(0); })
        .Property("y",
            [](const Vec3& s, ejsc::Context& c) { return ejsc::Value::Number(c, s.y); },
            [](Vec3& s, ejsc::Context&, const ejsc::Value& v) { s.y = v.ToNumber().value_or(0); })
        .Property("z",
            [](const Vec3& s, ejsc::Context& c) { return ejsc::Value::Number(c, s.z); },
            [](Vec3& s, ejsc::Context&, const ejsc::Value& v) { s.z = v.ToNumber().value_or(0); })
        // Read-only computed property.
        .Property("length",
            [](const Vec3& s, ejsc::Context& c) { return ejsc::Value::Number(c, s.length()); })
        .Method("toString", [](Vec3& s, ejsc::Context& c, auto) {
            return ejsc::Value::String(c, s.str());
        })
        .Build();

    ctx.SetGlobal("Vec3", Vec3Cls.ConstructorValue());

    ctx.SetGlobal("getPlayerPosition", ejsc::Value::Function(ctx, "getPlayerPosition",
        [Vec3Cls](ejsc::Context&, const ejsc::Value&, std::span<const ejsc::Value>) {
            return Vec3Cls.Wrap(&g_playerPos);
        }));

    ctx.Eval(R"({
        const v = new Vec3(1, 2, 2);
        print('v =', v.toString(), 'len =', v.length);
        v.x = 10;
        v.y = v.y + 5;
        print('after assignment:', v.toString(), 'len =', v.length);

        // Read-only property: assignment throws.
        try { v.length = 999 } catch (e) { print('caught:', e.message) }

        // Borrowed pointer — mutations from JS land in the host's Vec3.
        const pp = getPlayerPosition();
        pp.x = -7;
        print('borrowed after mutation:', pp.toString());
    })", "vec3-demo.js");

    if (ctx.HasException()) {
        std::cerr << "Vec3 demo: " << ctx.TakeException().ToString().value_or("?") << '\n';
        return 1;
    }

    std::cout << "host sees g_playerPos.x = " << g_playerPos.x << '\n';

    // -----------------------------------------------------------------------
    // Entity / Player inheritance
    // -----------------------------------------------------------------------

    auto EntityCls = ctx.NewClass<Entity>("Entity")
        .Constructor([](ejsc::Context&, std::span<const ejsc::Value> args) -> Entity* {
            auto* e = new Entity;
            if (!args.empty()) e->name = args[0].ToString().value_or("");
            return e;
        })
        .Property("name",
            [](const Entity& s, ejsc::Context& c) { return ejsc::Value::String(c, s.name); },
            [](Entity& s, ejsc::Context&, const ejsc::Value& v) {
                s.name = v.ToString().value_or("");
            })
        .Property("x",
            [](const Entity& s, ejsc::Context& c) { return ejsc::Value::Number(c, s.x); },
            [](Entity& s, ejsc::Context&, const ejsc::Value& v) { s.x = v.ToNumber().value_or(0); })
        .Property("y",
            [](const Entity& s, ejsc::Context& c) { return ejsc::Value::Number(c, s.y); },
            [](Entity& s, ejsc::Context&, const ejsc::Value& v) { s.y = v.ToNumber().value_or(0); })
        .Method("describe", [](Entity& s, ejsc::Context& c, auto) {
            return ejsc::Value::String(c, s.describe());
        })
        .Build();

    auto PlayerCls = ctx.NewClass<Player>("Player")
        .Extends(EntityCls)
        .Constructor([](ejsc::Context&, std::span<const ejsc::Value> args) -> Player* {
            auto* p = new Player;
            if (!args.empty()) p->name = args[0].ToString().value_or("");
            return p;
        })
        .Property("health",
            [](const Player& s, ejsc::Context& c) { return ejsc::Value::Number(c, s.health); },
            [](Player& s, ejsc::Context&, const ejsc::Value& v) {
                s.health = static_cast<int>(v.ToNumber().value_or(0));
            })
        .Method("damage", [](Player& s, ejsc::Context& c, std::span<const ejsc::Value> args) {
            s.health -= static_cast<int>(args[0].ToNumber().value_or(0));
            return ejsc::Value::Undefined(c);
        })
        .Build();

    ctx.SetGlobal("Entity", EntityCls.ConstructorValue());
    ctx.SetGlobal("Player", PlayerCls.ConstructorValue());

    ctx.Eval(R"({
        const player = new Player('Arthur');
        player.x = 10; player.y = 20;       // inherited setters from Entity
        print(player.describe(), 'health:', player.health);
        player.damage(35);
        print('after damage:', player.describe(), 'health:', player.health);

        print('player instanceof Player =', player instanceof Player);
        print('player instanceof Entity =', player instanceof Entity);
    })", "inheritance-demo.js");

    if (ctx.HasException()) {
        std::cerr << "Inheritance demo: " << ctx.TakeException().ToString().value_or("?") << '\n';
        return 1;
    }

    // C++ side: Unwrap a Player value as either Player* or Entity*.
    auto playerVal = ctx.Eval("new Player('Ada')", "<unwrap-test>");
    Entity* asEntity = EntityCls.Unwrap(playerVal);
    Player* asPlayer = PlayerCls.Unwrap(playerVal);
    std::cout << "Unwrap-as-Entity name: " << (asEntity ? asEntity->name : "<null>") << '\n';
    std::cout << "Unwrap-as-Player health: " << (asPlayer ? asPlayer->health : -1) << '\n';

    // Reverse: a plain Entity value should NOT unwrap as Player.
    auto entityVal = ctx.Eval("new Entity('Wall')", "<unwrap-test2>");
    std::cout << "PlayerCls.Unwrap(entityVal) = "
              << (PlayerCls.Unwrap(entityVal) ? "<unexpected>" : "nullptr (correct)") << '\n';

    return 0;
}
