# EmbeddedJSC — Plan

Standalone embedding library that wraps WebKit's JavaScriptCore (built from the
vcpkg overlay port originally developed in `rdr2scripthook`) and exposes a
QuickJS-class C++ API for hosts that want to embed JS with native modules.

## Goals

- Zero JS bootstrap: embedder writes only C++; the library does not parse any
  JS-side glue to expose native modules.
- QuickJS-class developer experience for: creating runtimes/contexts,
  registering native functions, registering native ES modules, evaluating
  scripts/modules, calling JS from C++.
- Same tech stack as the parent project — clang-cl, C++20, vcpkg manifest mode,
  CMakePresets, static MSVC runtime, Ninja.
- Ship a working example: a `math` native module imported via
  `import { add, sub } from 'math'`.

## Non-goals (v1)

- Cross-platform — Windows-only first; Linux later.
- Promise/microtask scheduling beyond `drainMicrotasks()` exposure.
- DOM/browser-style APIs.
- Class registration with prototype chains (v2).

## Decisions

| Topic            | Decision |
|------------------|----------|
| Location         | Sibling repo: `g:\Sources\EmbeddedJSC` |
| Module strategy  | True synthetic modules via JSC internals (no JS source emitted, even internally) |
| API style        | C++20 RAII wrapper (`Runtime` / `Context` / `Value` / `ModuleBuilder`) |
| Build            | CMake + vcpkg manifest, clang-cl, static MSVC runtime, Ninja |
| JSC source       | Same custom vcpkg overlay port as parent project, copied into this repo |

## Repo layout

```
CMakeLists.txt
CMakePresets.json
vcpkg.json                          # depends on "javascriptcore"
vcpkg-ports/javascriptcore/         # copied verbatim from parent repo
.gitignore
LICENSE
README.md
PLAN.md
include/ejsc/
    ejsc.h                          # umbrella include
    runtime.h                       # Runtime
    context.h                       # Context
    value.h                         # Value (handle, scoped to a Context)
    module.h                        # ModuleBuilder
    error.h                         # Exception / Result types
    fwd.h                           # forward decls
src/
    runtime.cpp
    context.cpp
    value.cpp
    module.cpp
    internal/
        global_object_table.{h,cpp} # custom GlobalObjectMethodTable
        module_loader.{h,cpp}       # resolve/fetch/evaluate hooks
        synthetic_module.{h,cpp}    # AbstractModuleRecord subclass / synthetic dispatch
        jsc_init.{h,cpp}            # one-shot WTF/JSC initialization
        scope.h                     # JSLockHolder + VM helpers
examples/
    hello/
        CMakeLists.txt
        main.cpp                    # ctx.Eval("print('hello')")
    native_module/
        CMakeLists.txt
        main.cpp                    # registers `math`, evaluates test.js
        test.js
tests/
    CMakeLists.txt
    test_eval.cpp
    test_native_module.cpp
```

## Public API sketch

```cpp
namespace ejsc {

class Runtime {
public:
    Runtime();
    ~Runtime();
    Context NewContext();
};

class Context {
public:
    ModuleBuilder NewModule(std::string_view name);

    Value Eval(std::string_view code, std::string_view filename = "<eval>");
    Value EvalModule(std::string_view code, std::string_view key);

    void  SetGlobal(std::string_view name, const Value& v);
    Value GetGlobal(std::string_view name);

    void  DrainMicrotasks();

    bool  HasException() const;
    Value TakeException();
};

class Value {
public:
    static Value Undefined(Context&);
    static Value Null(Context&);
    static Value Bool(Context&, bool);
    static Value Number(Context&, double);
    static Value String(Context&, std::string_view);
    static Value Object(Context&);

    using NativeFn = std::function<Value(Context&,
                                         const Value& thisVal,
                                         std::span<const Value> args)>;
    static Value Function(Context&, std::string_view name, NativeFn fn);

    bool IsUndefined() const; bool IsNull() const; bool IsBool() const;
    bool IsNumber() const;    bool IsString() const; bool IsObject() const;
    bool IsFunction() const;

    std::optional<bool>        ToBool()   const;
    std::optional<double>      ToNumber() const;
    std::optional<std::string> ToString() const;

    void  SetProperty(std::string_view name, const Value& v);
    Value GetProperty(std::string_view name) const;
    Value Call(const Value& thisVal, std::span<const Value> args) const;
};

class ModuleBuilder {
public:
    ModuleBuilder& Export(std::string_view name, Value v);
    ModuleBuilder& ExportFunction(std::string_view name, Value::NativeFn fn);
    void Build();   // commits the module to the context's loader
};

} // namespace ejsc
```

Sample embedder contract:

```cpp
ejsc::Runtime rt;
auto ctx = rt.NewContext();
auto mod = ctx.NewModule("math");
mod.ExportFunction("add", [](auto& c, auto, auto args) {
    return ejsc::Value::Number(c, *args[0].ToNumber() + *args[1].ToNumber());
});
mod.Build();
ctx.EvalModule("import { add } from 'math'; print(add(2,3));", "main.mjs");
```

## Synthetic-module implementation strategy

JSC has no public "synthetic module" C API. Plan:

- **Spike first.** Use `customModuleLoaderFetch` to recognise registered native
  modules. For native modules, try `SourceProviderSourceType::Synthetic` if it
  exists in our JSC build; otherwise build a `JSC::SyntheticModuleRecord` (or
  equivalent subclass of `AbstractModuleRecord`) directly via internal headers.
- **Storage.** A `Context`-owned `unordered_map<string, NativeModule>` maps
  module names to descriptor objects holding exports.
- **Resolve.** `moduleLoaderResolve` returns the name as-is for native modules.
- **Evaluate.** `moduleLoaderEvaluate` for a synthetic module instantiates the
  namespace object directly from the descriptor — no JS code runs.
- **Materialization.** For each native export, create the corresponding
  `JSObject*` (function or value) under the JSLock and bind it on the module
  record so `import { foo } from 'mod'` resolves through JSC's normal linker.
- **Fallback only if pure-synthetic is infeasible:** a hidden one-line
  `export default __ejsc_module_X__;` stub generated entirely inside the
  library. The embedder-facing API does not change. Used as escape valve only.

## Init / lifetime

- `JSC::Config::enableRestrictedOptions()`, `WTF::initializeMainThread()`,
  `JSC::initialize()`, `JSC::Options` config — modelled on the existing
  `JSRuntime::Initialize` and guarded by `std::once_flag` in `jsc_init.cpp`.
- `Runtime` holds nothing per-instance beyond marking initialization done.
- `Context` owns: `RefPtr<JSC::VM>`, the `JSGlobalObject*`, the native-module
  map, and a callback registry that keeps `std::function`s alive for the life
  of the context.
- `Value` is a handle: `JSValueRef` + parent `Context*`. Copy = `JSValueProtect`;
  destroy = `JSValueUnprotect`. Single-threaded; not RefCounted.

## Build system

- Root `CMakeLists.txt`: builds `libejsc` static lib + examples + tests.
- `CMakePresets.json`: `windows-debug` / `windows-release`, identical structure
  to parent (clang-cl, x64-windows-static, vcpkg toolchain, `/FI cmakeconfig.h`).
- `vcpkg.json`: depends on `javascriptcore`. The custom port is copied from
  `rdr2scripthook\vcpkg-ports\javascriptcore` so EmbeddedJSC is self-sufficient.
- vcpkg is not vendored; consumers add it via submodule themselves.

## Order of work (tracked)

- [x] **1. Scaffold repo + CMake/vcpkg config; build empty `libejsc`**
- [x] **2. JSC one-time init (`jsc_init.{h,cpp}`) with `std::once_flag`**
- [x] **3. `Runtime` + `Context` + plain script `Eval`; `examples/hello` passes**
- [x] **4. `Value` primitives + function binding via `JSObjectMakeFunctionWithCallback` + callback registry**
- [x] **5. Add `vcpkg` as a git submodule (Microsoft/vcpkg); switch CMakePresets to in-tree toolchain**
- [x] **6. ES module evaluation path with `moduleLoaderResolve/fetch/evaluate` (source-based first)**
- [x] **7. Spike: pick exact synthetic-module API in this JSC version; commit findings**
- [x] **8. `ModuleBuilder` + `Context::NewModule` + synthetic-module dispatch in loader**
- [x] **9. `examples/native_module` passes (`import { add, sub } from 'math'`)**
- [x] **10. Polish README + tighten docs**

### Progress notes

- 2026-05-26: Library builds clean with clang-cl x64-windows-static. `ejsc_hello.exe` prints `hello, ejsc` / `result = 3`. `ejsc_test_eval.exe` passes 4/4 assertions (arithmetic, string concat, native-function call, exception capture). Native-module import currently rejects with "native module fetch unimplemented" — that's the work in steps 7–8.
- 2026-05-27: `vcpkg` added as a git submodule pinned at `e03dc9b29710050cd1018bc5674688108658d327` (vcpkg 2026.04.27-438). `cmake --preset windows-debug` configures, builds, and runs against the in-tree submodule. Tests still 4/4.
- 2026-05-27: Synthetic modules **working**. Mechanism: in the loader's `Fetch` hook, when the requested key matches a registered native module, return a `JSSourceCode` backed by a `JSC::SyntheticSourceProvider` whose generator fills `exportNames` / `exportValues` from the `NativeModuleEntry`. JSC's `moduleLoaderParseModule` already routes `SourceProviderSourceType::Synthetic` to `SyntheticModuleRecord::tryCreateWithExportNamesAndValues`. Zero JS bootstrap. `examples/native_module` prints `add(2,3) = 5` and `sub(10,4) = 6`. `test_eval` now covers `import { add } from 'math'` (5/5 passing).
- 2026-05-27: Gamedev integration pass. (a) `EvalModule` now reliably returns the module namespace by capturing the module record in the evaluate hook and falling back to `moduleLoader->getModuleNamespaceObject(...)` when `loadAndEvaluateModule`'s promise resolves to undefined. Hosts can hold the namespace and call `tick()` per frame. (b) New opt-in helper library `ejsc::extra` (target name `ejsc_extra`) under [extra/](extra/). First component: `TimerManager` — implements `setTimeout` / `setInterval` / `clearTimeout` / `clearInterval` on top of `std::chrono::steady_clock`, with microtask draining after each callback. (c) New `examples/timers/` driver. (d) [EMBEDDING.md](EMBEDDING.md) covers lifetime, per-frame integration, microtasks, threading, and the "you implement timers" contract. `test_eval` grew a 6th case asserting the namespace path; 6/6 passing.
- 2026-05-27: **Class binding API.** `Context::NewClass<T>("Foo")` returns a `ClassBuilder<T>` with `.Constructor(...)`, `.Method(...)`, `.Build()`. The returned `Class<T>` handle exposes `New()` (JS-owned, finalizer deletes the C++ object), `Wrap(T*)` (embedder-owned, finalizer no-op), `Unwrap(value) -> T*` (type-checked via JSC's `JSValueIsObjectOfClass`), and `ConstructorValue()` for installing as a global/module export. Method dispatch goes through a shared prototype object holding `Value::Function`s; instance prototype set on construction. `ClassData` is owned by the Context so JS can keep instantiating after the C++ `Class<T>` handle goes out of scope. Internals: type-erased layer in `src/internal/class_bridge.{h,cpp}` (`void*`-based `ClassData`, `InstanceData{ obj, owned, classData }`); templated `ClassBuilder<T>` / `Class<T>` in `include/ejsc/class.h` adapt strongly-typed user lambdas via captured casts. New `examples/classes/` demos `new Vec3(...)`, borrowed `getPlayerPosition()`, `describe()` round-trip. `test_eval` grew a 6th case (Box class with `set`/`get`, both `New` and `Wrap`, wrong-`this` exception). EMBEDDING.md gained a "Binding C++ classes" section. v1 caveats: no accessor properties, no inheritance, no static methods. 7/7 tests passing.

## Verification

1. `cmake --preset windows-debug` then `cmake --build --preset windows-debug`.
2. Run `examples/hello` — expect `hello` printed.
3. Run `examples/native_module` — expect `5` from `add(2,3)` and matching `sub` value.
4. Run `tests/` — all assertions pass.

## Out of scope (v1)

- Timers (`setTimeout` etc.)
- Promise rejection tracking, async stack tooling
- Class binding API
- TypedArray / ArrayBuffer helpers
