#pragma once

#include "synthetic_module.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <JavaScriptCore/JSBase.h>

// Forward-declared JSC++ types (full headers included in TUs that need them).
namespace JSC {
class JSGlobalObject;
class VM;
}

namespace ejsc::internal {

struct ClassData;   // defined in class_bridge.h

// Per-Context internal state. Owned by ejsc::Context via unique_ptr.
struct ContextState {
    // VM is held alive via the explicit ref taken in Context's constructor;
    // the destructor balances it.
    JSC::VM* vm = nullptr;
    JSC::JSGlobalObject* globalObject = nullptr;

    // C-API global context ref (borrowed; lives as long as the VM/global do).
    // Cached so other TUs don't have to round-trip through APICast helpers.
    JSGlobalContextRef ctxRef = nullptr;

    // Last exception thrown out of an Eval/Call. JSValueProtect'd; cleared on
    // TakeException.
    const struct OpaqueJSValue* pendingException = nullptr;

    // Module record from the most recent moduleLoaderEvaluate call.
    // JSValueProtect'd while held so EvalModule can retrieve the namespace
    // even when loadAndEvaluateModule returns undefined. Cleared on context
    // teardown.
    const struct OpaqueJSValue* lastModuleRecord = nullptr;

    // Native modules registered on this context, keyed by module name.
    std::unordered_map<std::string, std::unique_ptr<NativeModuleEntry>> nativeModules;

    // Per-context guard for nativeModules.
    std::mutex modulesMutex;

    // Classes registered via Context::NewClass<T>. Owned here so the
    // ClassData lives at least as long as the Context — instances created
    // before the embedder's Class<T> handle goes out of scope keep working.
    std::vector<std::unique_ptr<ClassData>> classes;
};

// Look up the ContextState for a given JSGlobalObject (registered/unregistered
// when a Context is constructed/destroyed).
ContextState* StateForGlobalObject(JSC::JSGlobalObject* globalObject);

// Registration helpers used by Context's ctor/dtor.
void RegisterContextState(JSC::JSGlobalObject* globalObject, ContextState* state);
void UnregisterContextState(JSC::JSGlobalObject* globalObject);

} // namespace ejsc::internal
