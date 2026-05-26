#pragma once

// Class<T> / ClassBuilder<T> — bind a C++ type to a JS constructor.
//
// Quick taste:
//
//     struct Counter { int n = 0; };
//
//     auto CounterCls = ctx.NewClass<Counter>("Counter")
//         .Constructor([](auto& c, auto args) -> Counter* {
//             auto* x = new Counter;
//             if (!args.empty()) x->n = args[0].ToNumber().value_or(0);
//             return x;
//         })
//         .Method("inc", [](Counter& self, auto& c, auto) {
//             return ejsc::Value::Number(c, ++self.n);
//         })
//         .Method("value", [](Counter& self, auto& c, auto) {
//             return ejsc::Value::Number(c, self.n);
//         })
//         .Build();
//
//     ctx.SetGlobal("Counter", CounterCls.ConstructorValue());
//     ctx.Eval("const c = new Counter(5); c.inc(); c.inc(); print(c.value())", "main.js");
//
// Ownership:
//   - New(...)  -> JS owns the instance; finalizer deletes it.
//   - Wrap(T*)  -> embedder owns; finalizer does not delete. Make sure the
//                  C++ object outlives any JS handle to it.
//   - Unwrap(v) -> T* if v is an instance of this class, nullptr otherwise.

#include "context.h"
#include "error.h"
#include "value.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
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

    // Returns nullptr unless `v` is an instance of *this* class.
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
    using ConstructorFn = std::function<T*(Context&, std::span<const Value>)>;
    using MethodFn      = std::function<Value(T&, Context&, std::span<const Value>)>;

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

    Class<T> Build() {
        if (m_built) throw Error("ejsc: ClassBuilder<" + m_name + "> already Built");
        m_built = true;

        // Erase the constructor closure: T* -> void*.
        std::function<void*(Context&, std::span<const Value>)> erasedCtor;
        if (m_ctor) {
            erasedCtor = [fn = std::move(m_ctor)]
                (Context& c, std::span<const Value> args) -> void* {
                    return static_cast<void*>(fn(c, args));
                };
        }

        // Destructor: always `delete static_cast<T*>(ptr)` so T's dtor runs.
        std::function<void(void*)> erasedDtor =
            [](void* p) { delete static_cast<T*>(p); };

        internal::ClassData* cd = internal::CreateClass(
            *m_ctx, std::move(m_name),
            std::move(erasedCtor), std::move(erasedDtor));

        for (auto& [name, fn] : m_methods) {
            auto erased = [fn = std::move(fn)]
                (void* selfPtr, Context& c, std::span<const Value> args) -> Value {
                    return fn(*static_cast<T*>(selfPtr), c, args);
                };
            internal::ClassAddMethod(*cd, std::move(name), std::move(erased));
        }
        internal::ClassFinalize(*cd);

        return Class<T>(cd);
    }

private:
    Context* m_ctx;
    std::string m_name;
    ConstructorFn m_ctor;
    std::vector<std::pair<std::string, MethodFn>> m_methods;
    bool m_built = false;
};

// Templated Context method definition (declared in context.h).
template<typename T>
ClassBuilder<T> Context::NewClass(std::string_view name) {
    return ClassBuilder<T>(*this, std::string(name));
}

} // namespace ejsc
