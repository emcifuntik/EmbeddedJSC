#pragma once

#include "fwd.h"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Use JSC's own opaque-pointer typedefs from the small public C header.
#include <JavaScriptCore/JSBase.h>

namespace ejsc {

class Value {
public:
    using NativeFn = std::function<Value(Context&,
                                         const Value& thisVal,
                                         std::span<const Value> args)>;

    Value() noexcept;
    ~Value();

    Value(const Value& other);
    Value(Value&& other) noexcept;
    Value& operator=(const Value& other);
    Value& operator=(Value&& other) noexcept;

    // Factories
    static Value Undefined(Context& ctx);
    static Value Null(Context& ctx);
    static Value Bool(Context& ctx, bool b);
    static Value Number(Context& ctx, double n);
    static Value String(Context& ctx, std::string_view s);
    static Value Object(Context& ctx);
    static Value Function(Context& ctx, std::string_view name, NativeFn fn);

    // Type checks
    bool IsUndefined() const noexcept;
    bool IsNull() const noexcept;
    bool IsBool() const noexcept;
    bool IsNumber() const noexcept;
    bool IsString() const noexcept;
    bool IsObject() const noexcept;
    bool IsFunction() const noexcept;

    // Conversions (return nullopt on failure / wrong type)
    std::optional<bool>        ToBool()   const;
    std::optional<double>      ToNumber() const;
    std::optional<std::string> ToString() const;

    // Object access
    void  SetProperty(std::string_view name, const Value& v);
    Value GetProperty(std::string_view name) const;

    // Call (this value must be a function)
    Value Call(const Value& thisVal, std::span<const Value> args) const;

    // Raw accessors for embedders that want to interop with JSC C API.
    Context* GetContext() const noexcept { return m_ctx; }
    JSValueRef GetRef() const noexcept { return m_ref; }

    // Internal: construct from an already-acquired JSValueRef (does not protect).
    // Public so that ejsc internals across TUs can build values; embedders should
    // prefer the factories above.
    static Value Adopt(Context& ctx, JSValueRef ref);

private:
    Value(Context* ctx, JSValueRef ref, bool protect);

    void release() noexcept;

    Context*   m_ctx = nullptr;
    JSValueRef m_ref = nullptr;
};

} // namespace ejsc
