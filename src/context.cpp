#include "ejsc/context.h"
#include "ejsc/error.h"
#include "ejsc/value.h"

#include "internal/class_bridge.h"
#include "internal/context_state.h"
#include "internal/global_object_table.h"
#include "internal/module_loader.h"

#include <algorithm>
#include <string>
#include <vector>

#include <JavaScriptCore/JavaScript.h>

#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/Completion.h>
#include <JavaScriptCore/DFGDoesGCCheck.h>
#include <JavaScriptCore/JSGlobalObject.h>
#include <JavaScriptCore/JSInternalPromise.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSModuleLoader.h>
#include <JavaScriptCore/JSModuleNamespaceObject.h>
#include <JavaScriptCore/JSPromise.h>
#include <JavaScriptCore/JSString.h>
#include <JavaScriptCore/SourceCode.h>
#include <JavaScriptCore/VM.h>

#include <wtf/URL.h>

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

void storeException(internal::ContextState* s, JSValueRef exc) {
    if (!s) return;
    if (s->pendingException) {
        JSValueUnprotect(s->ctxRef, s->pendingException);
        s->pendingException = nullptr;
    }
    if (exc) {
        JSValueProtect(s->ctxRef, exc);
        s->pendingException = exc;
    }
}

} // namespace

Context::Context() : m_state(std::make_unique<internal::ContextState>()) {
    using namespace JSC;

    auto* s = m_state.get();

    RefPtr<VM> vmPtr = VM::tryCreate(HeapType::Large);
    if (!vmPtr) {
        throw Error("ejsc: failed to create JSC VM");
    }
    vmPtr->refSuppressingSaferCPPChecking();
    s->vm = vmPtr.get();

    s->vm->setDoesGCExpectation(true, DFG::DoesGCCheck::Special::Uninitialized);
    s->vm->heap.acquireAccess();

    JSLockHolder locker(*s->vm);

    Structure* structure = JSGlobalObject::createStructure(*s->vm, jsNull());
    s->globalObject = JSGlobalObject::createWithCustomMethodTable(
        *s->vm, structure, &internal::GetGlobalObjectMethodTable());

    s->ctxRef = toGlobalRef(s->globalObject);
    if (!s->ctxRef) {
        throw Error("ejsc: failed to obtain JSGlobalContextRef");
    }

    internal::RegisterContextState(s->globalObject, s);
}

Context::~Context() {
    if (!m_state) return;
    auto* s = m_state.get();
    if (!s->vm || !s->globalObject) return;

    JSC::JSLockHolder locker(*s->vm);

    if (s->pendingException) {
        JSValueUnprotect(s->ctxRef, s->pendingException);
        s->pendingException = nullptr;
    }

    if (s->lastModuleRecord) {
        JSValueUnprotect(s->ctxRef, s->lastModuleRecord);
        s->lastModuleRecord = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(s->modulesMutex);
        s->nativeModules.clear();
    }

    // Release registered classes. This drops the prototype and constructor
    // Values and releases the JSClassRefs; instances still on the heap will
    // get their finalizer called when JSC tears the heap down.
    s->classes.clear();

    s->vm->drainMicrotasks();

    internal::UnregisterContextState(s->globalObject);

    s->globalObject = nullptr;
    s->ctxRef = nullptr;
    s->vm = nullptr;
}

Context::Context(Context&& other) noexcept = default;
Context& Context::operator=(Context&& other) noexcept = default;

ModuleBuilder Context::NewModule(std::string_view name) {
    return ModuleBuilder(*this, std::string(name));
}

Value Context::Eval(std::string_view code, std::string_view filename) {
    auto* s = m_state.get();
    JSC::JSLockHolder locker(*s->vm);

    JSStringHolder script(code);
    JSStringHolder url(filename);

    JSValueRef exc = nullptr;
    JSValueRef result = JSEvaluateScript(s->ctxRef, script, nullptr, url, 1, &exc);
    if (exc) {
        storeException(s, exc);
        return Value::Undefined(*this);
    }
    return Value::Adopt(*this, result);
}

Value Context::EvalModule(std::string_view code, std::string_view key) {
    using namespace JSC;
    auto* s = m_state.get();
    JSLockHolder locker(*s->vm);

    std::string keyStr(key);
    std::replace(keyStr.begin(), keyStr.end(), '/', '\\');

    WTF::String sourceString = WTF::String::fromUTF8(std::string(code).c_str());
    WTF::String moduleKeyString = WTF::String::fromUTF8(keyStr.c_str());

    SourceOrigin sourceOrigin(WTF::URL::fileURLWithFileSystemPath(moduleKeyString));
    SourceCode sourceCode = makeSource(
        sourceString,
        sourceOrigin,
        SourceTaintedOrigin::Untainted,
        moduleKeyString,
        WTF::TextPosition(),
        SourceProviderSourceType::Module);

    JSInternalPromise* promise = loadAndEvaluateModule(s->globalObject, sourceCode, jsUndefined());
    s->vm->drainMicrotasks();

    if (!promise) {
        return Value::Undefined(*this);
    }

    JSPromise::Status status = promise->status();
    if (status == JSPromise::Status::Rejected) {
        storeException(s, toRef(s->globalObject, promise->result()));
        return Value::Undefined(*this);
    }
    if (status == JSPromise::Status::Pending) {
        return Value::Undefined(*this);
    }

    JSValue resolved = promise->result();
    if (resolved.isObject()) {
        return Value::Adopt(*this, toRef(s->globalObject, resolved));
    }

    // Some JSC builds resolve loadAndEvaluateModule's promise with undefined
    // for source-based modules. Fall back to the captured module record.
    if (s->lastModuleRecord) {
        JSValue record = toJS(s->globalObject, s->lastModuleRecord);
        if (JSModuleLoader* loader = s->globalObject->moduleLoader()) {
            if (JSModuleNamespaceObject* ns = loader->getModuleNamespaceObject(s->globalObject, record)) {
                return Value::Adopt(*this, toRef(ns));
            }
        }
    }
    return Value::Undefined(*this);
}

void Context::SetGlobal(std::string_view name, const Value& v) {
    auto* s = m_state.get();
    JSC::JSLockHolder locker(*s->vm);
    JSObjectRef global = JSContextGetGlobalObject(s->ctxRef);
    JSStringHolder propName(name);
    JSObjectSetProperty(s->ctxRef, global, propName, v.GetRef(),
                        kJSPropertyAttributeNone, nullptr);
}

Value Context::GetGlobal(std::string_view name) {
    auto* s = m_state.get();
    JSC::JSLockHolder locker(*s->vm);
    JSObjectRef global = JSContextGetGlobalObject(s->ctxRef);
    JSStringHolder propName(name);
    JSValueRef v = JSObjectGetProperty(s->ctxRef, global, propName, nullptr);
    return Value::Adopt(*this, v);
}

void* Context::RawGlobalContextRef() const noexcept {
    // JSGlobalContextRef is `const struct OpaqueJSContext*` (JSC's typedef
    // convention). The pointee is opaque, so the const is informational —
    // strip it so embedders get a plain void* they can cast back.
    return m_state ? const_cast<void*>(reinterpret_cast<const void*>(m_state->ctxRef))
                   : nullptr;
}

void Context::DrainMicrotasks() {
    auto* s = m_state.get();
    JSC::JSLockHolder locker(*s->vm);
    s->vm->drainMicrotasks();
}

bool Context::HasException() const {
    auto* s = m_state.get();
    return s && s->pendingException != nullptr;
}

Value Context::TakeException() {
    auto* s = m_state.get();
    if (!s || !s->pendingException) {
        return Value::Undefined(*this);
    }
    JSValueRef exc = s->pendingException;
    s->pendingException = nullptr;
    Value result = Value::Adopt(*this, exc);
    JSValueUnprotect(s->ctxRef, exc);
    return result;
}

} // namespace ejsc
