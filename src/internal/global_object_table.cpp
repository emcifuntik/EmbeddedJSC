#include "global_object_table.h"
#include "module_loader.h"

#include <JavaScriptCore/GlobalObjectMethodTable.h>
#include <JavaScriptCore/JSGlobalObject.h>
#include <JavaScriptCore/RuntimeFlags.h>
#include <JavaScriptCore/Structure.h>

namespace ejsc::internal {

static bool supportsRichSourceInfo(const JSC::JSGlobalObject*) { return true; }
static bool shouldInterruptScript(const JSC::JSGlobalObject*) { return false; }
static JSC::RuntimeFlags javaScriptRuntimeFlags(const JSC::JSGlobalObject*) { return {}; }
static bool shouldInterruptScriptBeforeTimeout(const JSC::JSGlobalObject*) { return false; }
static JSC::Structure* trustedScriptStructure(JSC::JSGlobalObject*) { return nullptr; }

static void promiseRejectionTracker(JSC::JSGlobalObject*,
                                    JSC::JSPromise*,
                                    JSC::JSPromiseRejectionOperation) {
    // No-op for now.
}

const JSC::GlobalObjectMethodTable& GetGlobalObjectMethodTable() {
    static const JSC::GlobalObjectMethodTable table = {
        &supportsRichSourceInfo,
        &shouldInterruptScript,
        &javaScriptRuntimeFlags,
        nullptr, // queueMicrotaskToEventLoop
        &shouldInterruptScriptBeforeTimeout,
        nullptr, // moduleLoaderImportModule
        &ModuleLoaderResolve,
        &ModuleLoaderFetch,
        nullptr, // moduleLoaderCreateImportMetaProperties
        &ModuleLoaderEvaluate,
        &promiseRejectionTracker,
        nullptr, // reportUncaughtExceptionAtEventLoop
        nullptr, // currentScriptExecutionOwner
        nullptr, // scriptExecutionStatus
        nullptr, // reportViolationForUnsafeEval
        nullptr, // defaultLanguage
        nullptr, // compileStreaming
        nullptr, // instantiateStreaming
        nullptr, // deriveShadowRealmGlobalObject
        nullptr, // codeForEval
        nullptr, // canCompileStrings
        &trustedScriptStructure,
    };
    return table;
}

} // namespace ejsc::internal
