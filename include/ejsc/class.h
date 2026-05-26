#pragma once

// Class<T> / ClassBuilder<T> — bind a C++ type to a JS constructor with
// methods, accessor properties, and (single) inheritance.
//
// Quick taste:
//
//     struct Vec3 { double x, y, z; };
//
//     auto Vec3Cls = ctx.NewClass<Vec3>("Vec3")
//         .Constructor([](auto& c, auto args) -> Vec3* {
//             auto* v = new Vec3;
//             if (args.size() > 0) v->x = args[0].ToNumber().value_or(0);
//             if (args.size() > 1) v->y = args[1].ToNumber().value_or(0);
//             if (args.size() > 2) v->z = args[2].ToNumber().value_or(0);
//             return v;
//         })
//         .Property("x",
//             [](const Vec3& s, auto& c) { return ejsc::Value::Number(c, s.x); },
//             [](Vec3& s, auto& c, const auto& v) { s.x = v.ToNumber().value_or(0); })
//         .Method("length", [](Vec3& s, auto& c, auto) {
//             return ejsc::Value::Number(c, std::sqrt(s.x*s.x + s.y*s.y + s.z*s.z));
//         })
//         .Build();
//
//     ctx.SetGlobal("Vec3", Vec3Cls.ConstructorValue());
//     ctx.Eval("const v = new Vec3(1,2,2); v.x = 10; print(v.x, v.length())");
//
// Inheritance (single):
//
//     struct Entity { ... };
//     struct Player : Entity { int health; };
//
//     auto EntityCls = ctx.NewClass<Entity>("Entity")...Build();
//     auto PlayerCls = ctx.NewClass<Player>("Player")
//         .Extends(EntityCls)
//         .Constructor(...)
//         .Property("health", ...)
//         .Build();
//
//     // EntityCls.Unwrap(playerValue) returns a valid Entity* (cast up the chain).
//     // playerValue instanceof EntityCls.Constructor() is true.
//
// Caveats:
//   - Single inheritance only. Don't register a C++ class with multiple base
//     classes as a parent — pick one.
//   - The cast chain uses static_cast<Parent*>(static_cast<T*>(p)), which
//     handles non-zero base offsets correctly for normal inheritance but is
//     not safe for virtual inheritance.
//   - Properties win over methods of the same name (`getProperty` runs before
//     prototype lookup). Don't register both with the same key.

#include "context.h"
#include "error.h"
#include "value.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ejsc {

namespace internal {
struct ClassData;

ClassData* CreateClass(Context& ctx, std::string name,
                       std::function<void*(Context&, std::span<const Value>)> ctor,
                       std::function<void(void*)> dtor);
void  ClassAddMethod(ClassData& cd, std::string methodName,
                     std::function<Value(void*, Context&, std::span<const Value>)> fn);
void  ClassAddProperty(ClassData& cd, std::string name,
                       std::function<Value(void*, Context&)> getter,
                       std::function<void(void*, Context&, const Value&)> setter);
void  ClassSetParent(ClassData& cd, ClassData* parent,
                     std::function<void*(void*)> castToParent);
void  ClassFinalize(ClassData& cd);

Value ClassNew(ClassData& cd, std::span<const Value> args);
Value ClassWrap(ClassData& cd, void* ptr);
void* ClassUnwrap(const ClassData& cd, const Value& v);
bool  ClassIsInstance(const ClassData& cd, const Value& v);
Value ClassConstructorValue(const ClassData& cd);
} // namespace internal

template<typename T>
class Class {
public:
    using Type = T;

    Class() noexcept = default;

    // Create a new instance, JS-owned (finalizer deletes via `delete static_cast<T*>(...)`).
    Value New(std::span<const Value> args = {}) const {
        require();
        return internal::ClassNew(*m_data, args);
    }
    Value New(std::initializer_list<Value> args) const {
        require();
        std::vector<Value> v(args);
        return internal::ClassNew(*m_data, std::span<const Value>(v.data(), v.size()));
    }

    // Wrap an externally-owned pointer; finalizer will NOT delete it.
    Value Wrap(T* obj) const {
        require();
        return internal::ClassWrap(*m_data, static_cast<void*>(obj));
    }

    // Returns nullptr unless `v` is an instance of *this* class (or a
    // class derived from it via Extends). For derived instances, the
    // returned pointer is correctly cast to T*.
    T* Unwrap(const Value& v) const {
        if (!m_data) return nullptr;
        return static_cast<T*>(internal::ClassUnwrap(*m_data, v));
    }

    bool IsInstance(const Value& v) const {
        return m_data && internal::ClassIsInstance(*m_data, v);
    }

    // JS-side constructor function. Install as a global, or expose as a
    // module export.
    Value ConstructorValue() const {
        require();
        return internal::ClassConstructorValue(*m_data);
    }

    explicit operator bool() const noexcept { return m_data != nullptr; }

    // Exposed so derived classes can pass us into Extends().
    internal::ClassData* Data() const noexcept { return m_data; }

private:
    template<typename U> friend class ClassBuilder;
    explicit Class(internal::ClassData* d) : m_data(d) {}
    void require() const {
        if (!m_data) throw Error("ejsc: Class<> handle is not bound (was Build() called?)");
    }

    internal::ClassData* m_data = nullptr;
};

template<typename T>
class ClassBuilder {
public:
    using ConstructorFn  = std::function<T*(Context&, std::span<const Value>)>;
    using MethodFn       = std::function<Value(T&, Context&, std::span<const Value>)>;
    using PropertyGetter = std::function<Value(const T&, Context&)>;
    using PropertySetter = std::function<void(T&, Context&, const Value&)>;

    ClassBuilder(Context& ctx, std::string name)
        : m_ctx(&ctx), m_name(std::move(name)) {}

    ClassBuilder& Constructor(ConstructorFn fn) {
        m_ctor = std::move(fn);
        return *this;
    }

    ClassBuilder& Method(std::string_view name, MethodFn fn) {
        m_methods.emplace_back(std::string(name), std::move(fn));
        return *this;
    }

    // Read-only property: getter only.
    ClassBuilder& Property(std::string_view name, PropertyGetter getter) {
        m_properties.push_back(PropEntry{
            std::string(name), std::move(getter), PropertySetter{}
        });
        return *this;
    }

    // Read-write property: getter + setter.
    ClassBuilder& Property(std::string_view name,
                           PropertyGetter getter,
                           PropertySetter setter) {
        m_properties.push_back(PropEntry{
            std::string(name), std::move(getter), std::move(setter)
        });
        return *this;
    }

    // Single inheritance. Parent must already be Built().
    template<typename Parent>
    ClassBuilder& Extends(const Class<Parent>& parent) {
        static_assert(std::is_base_of_v<Parent, T>,
                      "ejsc::ClassBuilder<T>::Extends<Parent>(): T must derive from Parent");
        if (!parent.Data()) {
            throw Error("ejsc: Extends() given an unbuilt parent class");
        }
        m_parent = parent.Data();
        m_castToParent = [](void* p) -> void* {
            // Round-trip through T to honour any base-class offset.
            return static_cast<void*>(static_cast<Parent*>(static_cast<T*>(p)));
        };
        return *this;
    }

    Class<T> Build() {
        if (m_built) throw Error("ejsc: ClassBuilder<" + m_name + "> already Built");
        m_built = true;

        std::function<void*(Context&, std::span<const Value>)> erasedCtor;
        if (m_ctor) {
            erasedCtor = [fn = std::move(m_ctor)]
                (Context& c, std::span<const Value> args) -> void* {
                    return static_cast<void*>(fn(c, args));
                };
        }

        std::function<void(void*)> erasedDtor =
            [](void* p) { delete static_cast<T*>(p); };

        internal::ClassData* cd = internal::CreateClass(
            *m_ctx, std::move(m_name),
            std::move(erasedCtor), std::move(erasedDtor));

        if (m_parent) {
            internal::ClassSetParent(*cd, m_parent, std::move(m_castToParent));
        }

        for (auto& [name, fn] : m_methods) {
            auto erased = [fn = std::move(fn)]
                (void* selfPtr, Context& c, std::span<const Value> args) -> Value {
                    return fn(*static_cast<T*>(selfPtr), c, args);
                };
            internal::ClassAddMethod(*cd, std::move(name), std::move(erased));
        }

        for (auto& e : m_properties) {
            auto erasedG = [g = std::move(e.getter)]
                (void* selfPtr, Context& c) -> Value {
                    return g(*static_cast<const T*>(selfPtr), c);
                };
            std::function<void(void*, Context&, const Value&)> erasedS;
            if (e.setter) {
                erasedS = [s = std::move(e.setter)]
                    (void* selfPtr, Context& c, const Value& v) {
                        s(*static_cast<T*>(selfPtr), c, v);
                    };
            }
            internal::ClassAddProperty(*cd, std::move(e.name),
                                       std::move(erasedG), std::move(erasedS));
        }

        internal::ClassFinalize(*cd);
        return Class<T>(cd);
    }

private:
    struct PropEntry {
        std::string name;
        PropertyGetter getter;
        PropertySetter setter;
    };

    Context* m_ctx;
    std::string m_name;
    ConstructorFn m_ctor;
    std::vector<std::pair<std::string, MethodFn>> m_methods;
    std::vector<PropEntry> m_properties;
    internal::ClassData* m_parent = nullptr;
    std::function<void*(void*)> m_castToParent;
    bool m_built = false;
};

// Templated Context method definition (declared in context.h).
template<typename T>
ClassBuilder<T> Context::NewClass(std::string_view name) {
    return ClassBuilder<T>(*this, std::string(name));
}

} // namespace ejsc
