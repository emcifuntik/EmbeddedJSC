#include "module_loader.h"
#include "context_state.h"

#include <mutex>
#include <unordered_map>

#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/Completion.h>
#include <JavaScriptCore/Identifier.h>
#include <JavaScriptCore/JSGlobalObject.h>
#include <JavaScriptCore/JSInternalPromise.h>
#include <JavaScriptCore/JSModuleLoader.h>
#include <JavaScriptCore/JSSourceCode.h>
#include <JavaScriptCore/JSString.h>
#include <JavaScriptCore/SourceCode.h>
#include <JavaScriptCore/Symbol.h>
#include <JavaScriptCore/VM.h>

#include <wtf/URL.h>

namespace ejsc::internal {

namespace {

// Map of JSGlobalObject -> ContextState. Kept in this TU so the loader and the
// Context can both reach the registry without exposing it publicly.
std::unordered_map<JSC::JSGlobalObject*, ContextState*>& StateMap() {
    static std::unordered_map<JSC::JSGlobalObject*, ContextState*> m;
    return m;
}
std::mutex& StateMapMutex() {
    static std::mutex m;
    return m;
}

} // namespace

void RegisterContextState(JSC::JSGlobalObject* globalObject, ContextState* state) {
    std::lock_guard<std::mutex> lock(StateMapMutex());
    StateMap()[globalObject] = state;
}

void UnregisterContextState(JSC::JSGlobalObject* globalObject) {
    std::lock_guard<std::mutex> lock(StateMapMutex());
    StateMap().erase(globalObject);
}

ContextState* StateForGlobalObject(JSC::JSGlobalObject* globalObject) {
    std::lock_guard<std::mutex> lock(StateMapMutex());
    auto it = StateMap().find(globalObject);
    return (it == StateMap().end()) ? nullptr : it->second;
}

JSC::Identifier ModuleLoaderResolve(JSC::JSGlobalObject* globalObject,
                                    JSC::JSModuleLoader*,
                                    JSC::JSValue key,
                                    JSC::JSValue /*referrer*/,
                                    JSC::JSValue /*scriptFetcher*/) {
    JSC::VM& vm = globalObject->vm();

    if (key.isSymbol()) {
        // Internal symbol key (used by JSC to track an already-known module).
        return JSC::Identifier::fromUid(JSC::asSymbol(key)->privateName());
    }
    if (key.isString()) {
        return JSC::Identifier::fromString(vm, key.toWTFString(globalObject));
    }
    return {};
}

JSC::JSInternalPromise* ModuleLoaderFetch(JSC::JSGlobalObject* globalObject,
                                          JSC::JSModuleLoader*,
                                          JSC::JSValue key,
                                          JSC::JSValue /*parameters*/,
                                          JSC::JSValue /*scriptFetcher*/) {
    JSC::VM& vm = globalObject->vm();
    auto* promise = JSC::JSInternalPromise::create(vm, globalObject->internalPromiseStructure());

    if (key.isSymbol()) {
        // The main module's source is provided inline via loadAndEvaluateModule,
        // so a fetch by symbol means JSC is asking for a module that we have
        // no source for.
        promise->reject(vm, globalObject,
                        JSC::createError(globalObject, "ejsc: cannot fetch module by symbol key"_s));
        return promise;
    }

    if (!key.isString()) {
        promise->reject(vm, globalObject,
                        JSC::createError(globalObject, "ejsc: invalid module key type"_s));
        return promise;
    }

    WTF::String keyStr = key.toWTFString(globalObject);
    std::string keyUtf8 = keyStr.utf8().data();

    // Native module path is handled in the synthetic-module spike (step 6).
    // For now, every non-symbol fetch is a miss until the spike fills this in.
    ContextState* state = StateForGlobalObject(globalObject);
    if (state) {
        std::lock_guard<std::mutex> lock(state->modulesMutex);
        if (state->nativeModules.count(keyUtf8)) {
            // TODO(spike): turn nativeModules[keyUtf8] into a JSSourceCode for a
            // synthetic module, or build the AbstractModuleRecord directly.
            promise->reject(vm, globalObject,
                            JSC::createError(globalObject,
                                             WTF::makeString("ejsc: native module fetch unimplemented: "_s, keyStr)));
            return promise;
        }
    }

    promise->reject(vm, globalObject,
                    JSC::createError(globalObject, WTF::makeString("ejsc: module not found: "_s, keyStr)));
    return promise;
}

JSC::JSValue ModuleLoaderEvaluate(JSC::JSGlobalObject* globalObject,
                                  JSC::JSModuleLoader* moduleLoader,
                                  JSC::JSValue key,
                                  JSC::JSValue moduleRecord,
                                  JSC::JSValue scriptFetcher,
                                  JSC::JSValue sentValue,
                                  JSC::JSValue resumeMode) {
    // For source-based modules (including the main module provided via
    // loadAndEvaluateModule), JSC's default evaluator is what we want.
    return moduleLoader->evaluateNonVirtual(globalObject, key, moduleRecord,
                                            scriptFetcher, sentValue, resumeMode);
}

} // namespace ejsc::internal
