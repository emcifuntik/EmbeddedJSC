#pragma once

#include "fwd.h"
#include "value.h"
#include "module.h"

#include <memory>
#include <string>
#include <string_view>

namespace ejsc {

namespace internal { struct ContextState; }

template<typename T> class ClassBuilder;

class Context {
public:
    Context();
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&& other) noexcept;
    Context& operator=(Context&& other) noexcept;

    ModuleBuilder NewModule(std::string_view name);

    // Register a C++ class. Returns a builder; chain Constructor()/Method()
    // calls and finish with Build(). The full definition lives in
    // <ejsc/class.h> — include that to instantiate this template.
    template<typename T>
    ClassBuilder<T> NewClass(std::string_view name);

    Value Eval(std::string_view code, std::string_view filename = "<eval>");
    Value EvalModule(std::string_view code, std::string_view key);

    void  SetGlobal(std::string_view name, const Value& v);
    Value GetGlobal(std::string_view name);

    void  DrainMicrotasks();

    bool  HasException() const;
    Value TakeException();

    // Internal accessors
    internal::ContextState* State() noexcept { return m_state.get(); }
    const internal::ContextState* State() const noexcept { return m_state.get(); }

private:
    std::unique_ptr<internal::ContextState> m_state;
};

} // namespace ejsc
