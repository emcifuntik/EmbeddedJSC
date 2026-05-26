#include "jsc_init.h"

#include <mutex>

#include <JavaScriptCore/JavaScript.h>
#include <JavaScriptCore/JSCConfig.h>
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/Options.h>

#include <wtf/MainThread.h>
#include <wtf/Threading.h>

namespace ejsc::internal {

void EnsureJSCInitialized() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        JSC::Config::enableRestrictedOptions();
        WTF::initializeMainThread();
        JSC::initialize();

        JSC::Options::AllowUnfinalizedAccessScope scope;
        JSC::Options::useConcurrentJIT() = true;
        JSC::Options::useWasm() = true;
        JSC::Options::useSourceProviderCache() = true;
        JSC::Options::exposeInternalModuleLoader() = true;
        JSC::Options::useSharedArrayBuffer() = true;
        JSC::Options::useJIT() = true;
        JSC::Options::useBBQJIT() = true;
        JSC::Options::useJITCage() = false;
        JSC::Options::useShadowRealm() = true;
        JSC::Options::useV8DateParser() = true;
        JSC::Options::useMathSumPreciseMethod() = true;
        JSC::Options::evalMode() = false;
        JSC::Options::heapGrowthSteepnessFactor() = 1.0;
        JSC::Options::heapGrowthMaxIncrease() = 2.0;
        JSC::Options::useAsyncStackTrace() = true;
        JSC::Options::useExplicitResourceManagement() = true;
        JSC::Options::assertOptionsAreCoherent();
    });
}

} // namespace ejsc::internal
