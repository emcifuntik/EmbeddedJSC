# EmbeddedJSC

An embedding library for WebKit's JavaScriptCore that aims for a
QuickJS-class developer experience: register native functions, declare native
ES modules, evaluate scripts, all from C++ with zero JS bootstrap.

Status: **early scaffolding.** See [PLAN.md](PLAN.md) for the roadmap and
checkbox-tracked progress.

## Build (Windows, clang-cl)

Tech stack: C++20, CMake + vcpkg manifest mode, custom JSC overlay port,
clang-cl, static MSVC runtime, Ninja.

1. Clone with submodules:
   ```pwsh
   git clone --recursive <repo-url> EmbeddedJSC
   # or, in an existing clone:
   git submodule update --init
   ```
2. Bootstrap vcpkg once:
   ```pwsh
   .\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   ```
3. Configure and build:
   ```pwsh
   cmake --preset windows-debug
   cmake --build --preset windows-debug
   ```
4. Examples land in `BIN/Debug/` (`ejsc_hello.exe`, `ejsc_native_module.exe`).
   The `native_module` example must be run from the directory containing
   `test.js` (CMake copies it next to the executable).

## Quick taste

```cpp
ejsc::Runtime rt;
auto ctx = rt.NewContext();

auto mod = ctx.NewModule("math");
mod.ExportFunction("add", [](ejsc::Context& c, const ejsc::Value&,
                             std::span<const ejsc::Value> args) {
    return ejsc::Value::Number(c, *args[0].ToNumber() + *args[1].ToNumber());
});
mod.Build();

ctx.EvalModule("import { add } from 'math'; print(add(2,3));", "main.mjs");
```

## Layout

See [PLAN.md](PLAN.md).
