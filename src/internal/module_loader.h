#pragma once

namespace JSC {
class Identifier;
class JSGlobalObject;
class JSInternalPromise;
class JSModuleLoader;
class JSValue;
}

namespace ejsc::internal {

JSC::Identifier ModuleLoaderResolve(JSC::JSGlobalObject* globalObject,
                                    JSC::JSModuleLoader*,
                                    JSC::JSValue key,
                                    JSC::JSValue referrer,
                                    JSC::JSValue scriptFetcher);

JSC::JSInternalPromise* ModuleLoaderFetch(JSC::JSGlobalObject* globalObject,
                                          JSC::JSModuleLoader*,
                                          JSC::JSValue key,
                                          JSC::JSValue parameters,
                                          JSC::JSValue scriptFetcher);

JSC::JSValue ModuleLoaderEvaluate(JSC::JSGlobalObject* globalObject,
                                  JSC::JSModuleLoader* moduleLoader,
                                  JSC::JSValue key,
                                  JSC::JSValue moduleRecord,
                                  JSC::JSValue scriptFetcher,
                                  JSC::JSValue sentValue,
                                  JSC::JSValue resumeMode);

} // namespace ejsc::internal
