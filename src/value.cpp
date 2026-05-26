#include "ejsc/value.h"
#include "ejsc/context.h"
#include "ejsc/error.h"

#include "internal/context_state.h"

#include <string>
#include <vector>

#include <JavaScriptCore/JavaScript.h>

namespace ejsc {

namespace {

class JSStringHolder {
public:
    explicit JSStringHolder(std::string_view s)
        : m_str(JSStringCreateWithUTF8CString(std::string(s).c_str())) {}
    ~JSStringHolder() { if (m_str) JSStringRelease(m_str); }
    JSStringHolder(const JSStringHolder&) = delete;
    JSStringHolder& operator=(const JSStringHolder&) = delete;
    operator JSStringRef() const { return m_str; }
private:
    JSStringRef m_str;
};

std::string utf8FromJSString(JSStringRef s) {
    size_t bufSize = JSStringGetMaximumUTF8CStringSize(s);
    std::vector<char> buf(bufSize);
    JSStringGetUTF8CString(s, buf.data(), bufSize);
    return std::string(buf.data());
}

inline JSGlobalContextRef cref(Context& ctx) { return ctx.State()->ctxRef; }
inline JSGlobalContextRef cref(const Context& ctx) { return ctx.State()->ctxRef; }

// JSClass for native functions: holds a heap-allocated NativeFn closure in
// private data, dispatches calls to it, and frees on finalize.
struct NativeClosure {
    Value::NativeFn fn;
    Context* ctx; // raw, non-owning; the closure can't outlive the Context
};

JSValueRef nativeCallAsFunction(JSContextRef ctx,
                                JSObjectRef function,
                                JSObjectRef thisObject,
                                size_t argumentCount,
                                const JSValueRef arguments[],
                                JSValueRef* exception) {
    auto* closure = static_cast<NativeClosure*>(JSObjectGetPrivate(function));
    if (!closure || !closure->ctx) {
        return JSValueMakeUndefined(ctx);
    }
    try {
        std::vector<Value> args;
        args.reserve(argumentCount);
        for (size_t i = 0; i < argumentCount; ++i) {
            args.push_back(Value::Adopt(*closure->ctx, arguments[i]));
        }
        Value thisV = Value::Adopt(*closure->ctx, thisObject);
        Value result = closure->fn(*closure->ctx, thisV,
                                   std::span<const Value>(args.data(), args.size()));
        return result.GetRef();
    } catch (const std::exception& e) {
        if (exception) {
            JSStringHolder msg(e.what());
            JSValueRef msgVal = JSValueMakeString(ctx, msg);
            JSValueRef errArgs[] = { msgVal };
            *exception = JSObjectMakeError(ctx, 1, errArgs, nullptr);
        }
        return JSValueMakeUndefined(ctx);
    } catch (...) {
        if (exception) {
            JSStringHolder msg("ejsc: native function threw");
            JSValueRef msgVal = JSValueMakeString(ctx, msg);
            JSValueRef errArgs[] = { msgVal };
            *exception = JSObjectMakeError(ctx, 1, errArgs, nullptr);
        }
        return JSValueMakeUndefined(ctx);
    }
}

void nativeFinalize(JSObjectRef object) {
    auto* closure = static_cast<NativeClosure*>(JSObjectGetPrivate(object));
    delete closure;
    JSObjectSetPrivate(object, nullptr);
}

JSClassRef nativeFunctionClass() {
    static JSClassRef cls = [] {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = "EJSCNativeFunction";
        def.callAsFunction = nativeCallAsFunction;
        def.finalize = nativeFinalize;
        return JSClassCreate(&def);
    }();
    return cls;
}

} // namespace

// ---------------------------------------------------------------------------
// Value lifetime
// ---------------------------------------------------------------------------

Value::Value() noexcept = default;

Value::Value(Context* ctx, JSValueRef ref, bool protect)
    : m_ctx(ctx), m_ref(ref) {
    if (protect && m_ctx && m_ref) {
        JSValueProtect(m_ctx->State()->ctxRef, m_ref);
    }
}

Value::~Value() {
    release();
}

void Value::release() noexcept {
    if (m_ctx && m_ref) {
        JSValueUnprotect(m_ctx->State()->ctxRef, m_ref);
    }
    m_ctx = nullptr;
    m_ref = nullptr;
}

Value::Value(const Value& other) : Value(other.m_ctx, other.m_ref, /*protect=*/true) {}

Value::Value(Value&& other) noexcept
    : m_ctx(other.m_ctx), m_ref(other.m_ref) {
    other.m_ctx = nullptr;
    other.m_ref = nullptr;
}

Value& Value::operator=(const Value& other) {
    if (this != &other) {
        release();
        m_ctx = other.m_ctx;
        m_ref = other.m_ref;
        if (m_ctx && m_ref) {
            JSValueProtect(m_ctx->State()->ctxRef, m_ref);
        }
    }
    return *this;
}

Value& Value::operator=(Value&& other) noexcept {
    if (this != &other) {
        release();
        m_ctx = other.m_ctx;
        m_ref = other.m_ref;
        other.m_ctx = nullptr;
        other.m_ref = nullptr;
    }
    return *this;
}

Value Value::Adopt(Context& ctx, JSValueRef ref) {
    return Value(&ctx, ref, /*protect=*/true);
}

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

Value Value::Undefined(Context& ctx) {
    return Adopt(ctx, JSValueMakeUndefined(cref(ctx)));
}
Value Value::Null(Context& ctx) {
    return Adopt(ctx, JSValueMakeNull(cref(ctx)));
}
Value Value::Bool(Context& ctx, bool b) {
    return Adopt(ctx, JSValueMakeBoolean(cref(ctx), b));
}
Value Value::Number(Context& ctx, double n) {
    return Adopt(ctx, JSValueMakeNumber(cref(ctx), n));
}
Value Value::String(Context& ctx, std::string_view s) {
    JSStringHolder js(s);
    return Adopt(ctx, JSValueMakeString(cref(ctx), js));
}
Value Value::Object(Context& ctx) {
    return Adopt(ctx, JSObjectMake(cref(ctx), nullptr, nullptr));
}
Value Value::Function(Context& ctx, std::string_view /*name*/, NativeFn fn) {
    auto* closure = new NativeClosure{ std::move(fn), &ctx };
    JSObjectRef obj = JSObjectMake(cref(ctx), nativeFunctionClass(), closure);
    return Adopt(ctx, obj);
}

// ---------------------------------------------------------------------------
// Type checks
// ---------------------------------------------------------------------------

bool Value::IsUndefined() const noexcept {
    return !m_ref || JSValueIsUndefined(cref(*m_ctx), m_ref);
}
bool Value::IsNull() const noexcept {
    return m_ref && JSValueIsNull(cref(*m_ctx), m_ref);
}
bool Value::IsBool() const noexcept {
    return m_ref && JSValueIsBoolean(cref(*m_ctx), m_ref);
}
bool Value::IsNumber() const noexcept {
    return m_ref && JSValueIsNumber(cref(*m_ctx), m_ref);
}
bool Value::IsString() const noexcept {
    return m_ref && JSValueIsString(cref(*m_ctx), m_ref);
}
bool Value::IsObject() const noexcept {
    return m_ref && JSValueIsObject(cref(*m_ctx), m_ref);
}
bool Value::IsFunction() const noexcept {
    if (!IsObject()) return false;
    JSObjectRef obj = JSValueToObject(cref(*m_ctx), m_ref, nullptr);
    return obj && JSObjectIsFunction(cref(*m_ctx), obj);
}

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

std::optional<bool> Value::ToBool() const {
    if (!m_ref) return std::nullopt;
    return JSValueToBoolean(cref(*m_ctx), m_ref);
}
std::optional<double> Value::ToNumber() const {
    if (!m_ref) return std::nullopt;
    JSValueRef exc = nullptr;
    double n = JSValueToNumber(cref(*m_ctx), m_ref, &exc);
    if (exc) return std::nullopt;
    return n;
}
std::optional<std::string> Value::ToString() const {
    if (!m_ref) return std::nullopt;
    JSValueRef exc = nullptr;
    JSStringRef js = JSValueToStringCopy(cref(*m_ctx), m_ref, &exc);
    if (exc || !js) return std::nullopt;
    std::string s = utf8FromJSString(js);
    JSStringRelease(js);
    return s;
}

// ---------------------------------------------------------------------------
// Object access
// ---------------------------------------------------------------------------

void Value::SetProperty(std::string_view name, const Value& v) {
    if (!IsObject()) return;
    JSObjectRef obj = JSValueToObject(cref(*m_ctx), m_ref, nullptr);
    JSStringHolder propName(name);
    JSObjectSetProperty(cref(*m_ctx), obj, propName, v.GetRef(),
                        kJSPropertyAttributeNone, nullptr);
}

Value Value::GetProperty(std::string_view name) const {
    if (!IsObject()) return Undefined(*m_ctx);
    JSObjectRef obj = JSValueToObject(cref(*m_ctx), m_ref, nullptr);
    JSStringHolder propName(name);
    JSValueRef v = JSObjectGetProperty(cref(*m_ctx), obj, propName, nullptr);
    return Adopt(*m_ctx, v);
}

Value Value::Call(const Value& thisVal, std::span<const Value> args) const {
    if (!IsFunction()) return Undefined(*m_ctx);
    JSObjectRef fn = JSValueToObject(cref(*m_ctx), m_ref, nullptr);

    std::vector<JSValueRef> raw;
    raw.reserve(args.size());
    for (const auto& a : args) raw.push_back(a.GetRef());

    JSObjectRef thisObj = nullptr;
    if (thisVal.IsObject()) {
        thisObj = JSValueToObject(cref(*m_ctx), thisVal.GetRef(), nullptr);
    }

    JSValueRef exc = nullptr;
    JSValueRef result = JSObjectCallAsFunction(cref(*m_ctx), fn, thisObj,
                                               raw.size(), raw.empty() ? nullptr : raw.data(),
                                               &exc);
    if (exc) return Undefined(*m_ctx);
    return Adopt(*m_ctx, result);
}

} // namespace ejsc
