#include "module_loader.h"
#include "context_state.h"

#include <mutex>
#include <unordered_map>

#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/ArgList.h>
#include <JavaScriptCore/JavaScript.h>
#include <JavaScriptCore/Completion.h>
#include <JavaScriptCore/Identifier.h>
#include <JavaScriptCore/JSGlobalObject.h>
#include <JavaScriptCore/JSInternalPromise.h>
#include <JavaScriptCore/JSModuleLoader.h>
#include <JavaScriptCore/JSSourceCode.h>
#include <JavaScriptCore/JSString.h>
#include <JavaScriptCore/SourceCode.h>
#include <JavaScriptCore/SourceOrigin.h>
#include <JavaScriptCore/SourceProvider.h>
#include <JavaScriptCore/Symbol.h>
#include <JavaScriptCore/VM.h>

#include <wtf/Function.h>
#include <wtf/URL.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

namespace ejsc::internal {

namespace {

// Map of JSGlobalObject -> ContextState. Defined in this TU so the loader and
// Context's ctor/dtor both reach the registry without exposing it publicly.
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
        // Main module's source is provided inline via loadAndEvaluateModule.
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

    ContextState* state = StateForGlobalObject(globalObject);
    if (!state) {
        promise->reject(vm, globalObject,
                        JSC::createError(globalObject, "ejsc: context state missing"_s));
        return promise;
    }

    bool isNative = false;
    {
        std::lock_guard<std::mutex> lock(state->modulesMutex);
        isNative = state->nativeModules.count(keyUtf8) > 0;
    }

    if (!isNative) {
        promise->reject(vm, globalObject,
                        JSC::createError(globalObject, WTF::makeString("ejsc: module not found: "_s, keyStr)));
        return promise;
    }

    // Native module: build a synthetic source provider whose generator copies
    // names+values from the NativeModuleEntry at parse time.
    auto generator = [state, keyUtf8](JSC::JSGlobalObject* go,
                                      JSC::Identifier,
                                      WTF::Vector<JSC::Identifier, 4>& exportNames,
                                      JSC::MarkedArgumentBuffer& exportValues) {
        JSC::VM& vmInner = go->vm();
        std::lock_guard<std::mutex> lock(state->modulesMutex);
        auto it = state->nativeModules.find(keyUtf8);
        if (it == state->nativeModules.end()) return;
        for (const auto& [name, value] : it->second->exports) {
            exportNames.append(JSC::Identifier::fromString(vmInner, WTF::String::fromUTF8(name.c_str())));
            exportValues.append(::toJS(go, value.GetRef()));
        }
    };

    WTF::String moduleURLString = WTF::makeString("ejsc-native://"_s, keyStr);
    WTF::URL moduleURL(moduleURLString);
    JSC::SourceOrigin sourceOrigin(moduleURL);

    auto provider = JSC::SyntheticSourceProvider::create(std::move(generator), sourceOrigin, moduleURLString);
    JSC::SourceCode sourceCode(std::move(provider));

    promise->resolve(globalObject, JSC::JSSourceCode::create(vm, std::move(sourceCode)));
    return promise;
}

JSC::JSValue ModuleLoaderEvaluate(JSC::JSGlobalObject* globalObject,
                                  JSC::JSModuleLoader* moduleLoader,
                                  JSC::JSValue key,
                                  JSC::JSValue moduleRecord,
                                  JSC::JSValue scriptFetcher,
                                  JSC::JSValue sentValue,
                                  JSC::JSValue resumeMode) {
    // Capture the module record so Context::EvalModule can recover the
    // namespace object even when loadAndEvaluateModule's promise resolves to
    // undefined (current JSC behaviour for source-based modules).
    if (ContextState* state = StateForGlobalObject(globalObject)) {
        JSValueRef recordRef = ::toRef(globalObject, moduleRecord);
        JSGlobalContextRef ctxRef = ::toGlobalRef(globalObject);
        if (state->lastModuleRecord) {
            JSValueUnprotect(ctxRef, state->lastModuleRecord);
        }
        if (recordRef) {
            JSValueProtect(ctxRef, recordRef);
        }
        state->lastModuleRecord = recordRef;
    }

    // Default evaluator handles both source-based and synthetic module records.
    return moduleLoader->evaluateNonVirtual(globalObject, key, moduleRecord,
                                            scriptFetcher, sentValue, resumeMode);
}

} // namespace ejsc::internal
