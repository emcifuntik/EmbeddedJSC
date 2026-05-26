# Embedding EmbeddedJSC

A practical guide to wiring EmbeddedJSC into a real host — CLI tool, daemon,
or game engine. If you only want the elevator pitch, read [README.md](README.md);
this document is the full reference an embedder needs.

## Contents

1. [Mental model](#mental-model)
2. [Quickstart](#quickstart)
3. [Lifetime and ownership](#lifetime-and-ownership)
4. [Binding native functions](#binding-native-functions)
5. [Native ES modules](#native-es-modules)
6. [Binding C++ classes](#binding-c-classes)
7. [Calling JS from C++](#calling-js-from-c)
8. [Per-frame integration (game loops)](#per-frame-integration-game-loops)
9. [Microtasks](#microtasks)
10. [Timers — you must implement these](#timers--you-must-implement-these)
11. [Exception handling](#exception-handling)
12. [Threading and re-entrancy](#threading-and-re-entrancy)
13. [What is intentionally not provided](#what-is-intentionally-not-provided)

---

## Mental model

EmbeddedJSC wraps three JSC concepts:

| C++ class       | Wraps                                | Lives for…                          |
|-----------------|--------------------------------------|-------------------------------------|
| `ejsc::Runtime` | One-shot JSC + WTF initialization     | Whole process                       |
| `ejsc::Context` | A `JSC::VM` + a `JSGlobalObject`      | One mod / sandbox / script world    |
| `ejsc::Value`   | A `JSValueRef` (GC-protected by RAII) | As long as a C++ handle exists      |

A `Runtime` is essentially a marker that JSC has been initialized. The real
state lives inside each `Context`. Hosts typically create one `Context` per
isolation boundary — one per mod in a game, one per worker in a server, etc.

## Quickstart

```cpp
#include <ejsc/ejsc.h>
#include <iostream>

int main() {
    ejsc::Runtime rt;
    auto ctx = rt.NewContext();

    ctx.SetGlobal("print", ejsc::Value::Function(ctx, "print",
        [](ejsc::Context& c, const ejsc::Value&, std::span<const ejsc::Value> args) {
            for (auto& a : args)
                std::cout << a.ToString().value_or("?") << ' ';
            std::cout << '\n';
            return ejsc::Value::Undefined(c);
        }));

    ctx.Eval("print('hello'); 1 + 2", "main.js");
    if (ctx.HasException()) {
        std::cerr << ctx.TakeException().ToString().value_or("?") << '\n';
        return 1;
    }
}
```

Build with `cmake --preset windows-debug && cmake --build --preset windows-debug`.
See [README.md](README.md) for prerequisites.

## Lifetime and ownership

- **`Runtime` is movable-free and trivially cheap.** Create one, keep it for
  the life of the process. The JSC global state is initialized exactly once
  regardless of how many `Runtime`s you create.
- **`Context` owns a JSC `VM`.** Destroying a `Context` releases everything
  bound to it (native modules, callbacks, globals, namespaces).
- **`Value` is a handle.** Copying a `Value` calls `JSValueProtect`; destroying
  it calls `JSValueUnprotect`. Hold `Value`s as long as you need the
  underlying JS value to survive GC.
- **Values from one Context must not be passed to another.** There is no
  cross-context marshalling. If you need to share data across mods, marshal
  to plain C++ (strings, numbers, structs) at the boundary.
- **Pointers captured by lambdas must outlive the Context.** If a native
  function lambda captures `&someManager`, `someManager` must live at least
  as long as the Context. The simplest pattern: own host services in the
  same scope as the Runtime, create Contexts under them.

## Binding native functions

```cpp
auto add = ejsc::Value::Function(ctx, "add",
    [](ejsc::Context& c, const ejsc::Value& thisVal,
       std::span<const ejsc::Value> args) -> ejsc::Value {
        double a = args.size() > 0 ? args[0].ToNumber().value_or(0.0) : 0.0;
        double b = args.size() > 1 ? args[1].ToNumber().value_or(0.0) : 0.0;
        return ejsc::Value::Number(c, a + b);
    });
ctx.SetGlobal("add", add);
```

The callback signature is:

```cpp
using NativeFn = std::function<Value(Context&,
                                     const Value& thisVal,
                                     std::span<const Value> args)>;
```

`thisVal` is whatever JS passed as `this` (often `undefined`). If your
callback throws a C++ exception derived from `std::exception`, the library
catches it and rethrows it on the JS side as an `Error` whose message is
`e.what()`. Other C++ exceptions become an `Error("ejsc: native function threw")`.

## Native ES modules

This is the headline feature. Register a module entirely in C++:

```cpp
auto math = ctx.NewModule("math");
math.ExportFunction("add", [](ejsc::Context& c, auto, auto args) {
    return ejsc::Value::Number(c, *args[0].ToNumber() + *args[1].ToNumber());
});
math.Export("PI", ejsc::Value::Number(ctx, 3.14159265358979323846));
math.Build();
```

JS code can then do:

```js
import { add, PI } from 'math';
print(add(1, 2));
print(PI);
```

There is **no JS source generated** — not even internally. The library wires
your exports into a JSC `SyntheticSourceProvider`, which JSC then turns into
a `SyntheticModuleRecord` via its existing JSON/synthetic-module pipeline.

A few rules:

- `Build()` commits the module to the Context. Once called, the module is
  importable.
- A second `ModuleBuilder` with the same name replaces the previous entry
  (so you can hot-reload mod modules).
- Modules registered after `EvalModule` has already imported them are not
  re-imported. Register before you evaluate.
- Exports of any `Value` type are fine — objects, functions, primitives.

## Binding C++ classes

Sometimes you want JS to construct, inspect, and pass around your own C++
types — game entities, vectors, components. `Context::NewClass<T>()` builds
a JS constructor backed by a C++ type:

```cpp
struct Vec3 { double x, y, z; };

auto Vec3Cls = ctx.NewClass<Vec3>("Vec3")
    .Constructor([](ejsc::Context&, std::span<const ejsc::Value> args) -> Vec3* {
        auto* v = new Vec3;
        if (args.size() > 0) v->x = args[0].ToNumber().value_or(0.0);
        if (args.size() > 1) v->y = args[1].ToNumber().value_or(0.0);
        if (args.size() > 2) v->z = args[2].ToNumber().value_or(0.0);
        return v;
    })
    .Method("length", [](Vec3& self, ejsc::Context& c, auto) {
        return ejsc::Value::Number(c,
            std::sqrt(self.x * self.x + self.y * self.y + self.z * self.z));
    })
    .Build();

ctx.SetGlobal("Vec3", Vec3Cls.ConstructorValue());

ctx.Eval("const v = new Vec3(1, 2, 2); print(v.length())", "main.js");
// prints: 3
```

### Ownership

There are two ways an instance can enter the JS world:

| Source            | Owner                  | Finalize behaviour                        |
|-------------------|------------------------|--------------------------------------------|
| `new Foo(...)` from JS, or `cls.New(...)` | JS-owned (GC) | `delete static_cast<T*>(ptr)` runs |
| `cls.Wrap(ptr)`   | Embedder               | Finalizer does **not** delete              |

`Wrap` is what you want for objects the host already owns — a player entity,
a system singleton, a game world. The C++ object must outlive every JS
handle that references it; otherwise the JS side will be holding a dangling
pointer.

### From the C++ side

```cpp
// Hand a C++-owned object to JS.
Vec3 playerPos{ 100, 200, 50 };
ctx.SetGlobal("playerPos", Vec3Cls.Wrap(&playerPos));

// Create a new JS-owned instance from C++ and pass it around.
auto v = Vec3Cls.New({ ejsc::Value::Number(ctx, 1),
                       ejsc::Value::Number(ctx, 2),
                       ejsc::Value::Number(ctx, 2) });

// Unwrap a Value that came back from JS.
auto wrapped = ctx.GetGlobal("playerPos");
if (Vec3* p = Vec3Cls.Unwrap(wrapped)) {
    p->x += 1.0;     // mutations are visible to JS
}

// Type-check without unwrapping.
if (Vec3Cls.IsInstance(someValue)) { /* ... */ }
```

### Accessor properties

Use `Property(name, getter)` for read-only and `Property(name, getter,
setter)` for read-write. The getter receives `const T&`; the setter
receives `T&` and the assigned `Value`.

```cpp
.Property("x",
    [](const Vec3& s, ejsc::Context& c) { return ejsc::Value::Number(c, s.x); },
    [](Vec3& s, ejsc::Context&, const ejsc::Value& v) { s.x = v.ToNumber().value_or(0); })
.Property("length",          // read-only computed
    [](const Vec3& s, ejsc::Context& c) {
        return ejsc::Value::Number(c, std::sqrt(s.x*s.x + s.y*s.y + s.z*s.z));
    })
```

From JS:

```js
const v = new Vec3(1, 2, 2);
v.x = 10;                        // setter
console.log(v.x, v.length);      // getters
v.length = 999;                  // TypeError: ejsc: read-only property 'length'
```

Accessors take precedence over methods of the same name (the class-level
`getProperty` callback fires before prototype-chain lookup). Don't register
both with the same key.

### Inheritance

Single inheritance with `Extends`:

```cpp
struct Entity { std::string name; double x, y, z; };
struct Player : Entity { int health; };

auto EntityCls = ctx.NewClass<Entity>("Entity")
    .Constructor(...)
    .Property("name", ..., ...)
    .Property("x",    ..., ...)
    .Method("describe", ...)
    .Build();

auto PlayerCls = ctx.NewClass<Player>("Player")
    .Extends(EntityCls)                 // parent must be Built() first
    .Constructor(...)
    .Property("health", ..., ...)
    .Method("damage", ...)
    .Build();
```

What you get:

| Behaviour                                     | How                                                          |
|-----------------------------------------------|--------------------------------------------------------------|
| `player.x = 10` (inherited setter)            | accessor walk finds `x` on Entity, casts `Player*` to `Entity*` |
| `player.describe()` (inherited method)        | prototype chain: Player.prototype.__proto__ === Entity.prototype |
| `player instanceof Entity`                    | `Entity.prototype` is in `player`'s prototype chain          |
| `EntityCls.Unwrap(playerValue) -> Entity*`    | `JSValueIsObjectOfClass` walks `parentClass`; pointer is cast up |

Constraints:

- **Single inheritance only.** Don't try to model multiple base classes via
  `Extends`. Pick one parent.
- **The cast is `static_cast<Parent*>(static_cast<T*>(p))`.** Correct for
  normal inheritance (including non-zero base offsets). Not safe for virtual
  inheritance — avoid that pattern.
- **Parent must already be `Build()`-ed** before `Extends`. The API enforces
  this at runtime; passing an unbuilt `Class<Parent>` throws.

### Method `this`

A bound method that's called with the wrong `this` (e.g.
`obj.method.call({})`) raises a JS `Error` whose message identifies the
expected class and method name. Native callbacks throwing a `std::exception`
inside a method follow the same conversion as plain native functions —
they surface as JS errors with the C++ message.

### Lifetime of the class itself

The `Class<T>` handle is cheap to copy and doesn't own the class
registration. `ClassData` lives on the Context, so JS can keep instantiating
`new Foo()` long after the `Class<T>` C++ handle has gone out of scope.
Registration ends when the Context is destroyed.

### Not in v1

- **Multiple inheritance.** `Extends` accepts one parent. Compose by
  ownership (a Player *has* a Health, not *is a* Health) for everything else.
- **Virtual inheritance.** Cast chain uses `static_cast`, which is incorrect
  for virtual bases.
- **Static / class-level methods.** Attach them to the constructor function
  with `MyCls.ConstructorValue().SetProperty("staticName", fn)` after
  building, if you need them.
- **Symbol-keyed properties / `Symbol.iterator`.** Use methods for iteration
  helpers; full Symbol support is future work.

## Calling JS from C++

The host frequently wants to call into JS — once per frame, in response to
input, on a network event. `EvalModule` returns the module's namespace as a
`Value`, which you can hold and dispatch through:

```cpp
auto ns = ctx.EvalModule(modSource, "mymod.mjs");
if (ctx.HasException()) { /* ... */ }

// Keep the namespace alive for the lifetime of this mod.
ejsc::Value modNamespace = ns;

// Later, per frame:
auto tick = modNamespace.GetProperty("tick");
if (tick.IsFunction()) {
    tick.Call(ejsc::Value::Undefined(ctx), {});
    if (ctx.HasException()) {
        log(ctx.TakeException().ToString().value_or("<unknown>"));
    }
}
```

For an arbitrary global (e.g. a callback the JS registered via your own
`onTick(fn)` host API), retain the `Value` you received from JS and call
`v.Call(thisVal, args)` whenever you need to.

## Per-frame integration (game loops)

The recommended pattern for a host that needs to drive JS every frame:

```cpp
// Setup, once.
ejsc::Runtime rt;
auto ctx = rt.NewContext();

ejsc::extra::TimerManager timers(ctx);
timers.Install();
// ... register native modules, native globals, etc. ...

auto modNs = ctx.EvalModule(modSource, "mod.mjs");
auto tick  = modNs.GetProperty("tick");

// Per frame.
void onGameTick(uint32_t deltaMs) {
    if (tick.IsFunction()) {
        tick.Call(ejsc::Value::Undefined(ctx),
                  { ejsc::Value::Number(ctx, deltaMs) });
        if (ctx.HasException()) {
            log_error(ctx.TakeException().ToString().value_or("?"));
        }
    }
    timers.Tick();             // fire any due timers, drains microtasks
    ctx.DrainMicrotasks();     // catch any stragglers
}

void onKeyDown(uint32_t keyCode) {
    auto handler = modNs.GetProperty("onKeyDown");
    if (handler.IsFunction()) {
        handler.Call(ejsc::Value::Undefined(ctx),
                     { ejsc::Value::Number(ctx, keyCode) });
        ctx.DrainMicrotasks();
    }
}
```

Order of operations inside a frame:

1. Run host work that needs to happen before script (physics, network).
2. Call into JS (`tick`, event handlers).
3. **Always drain microtasks** after the last JS call. Otherwise
   `await`/`.then(...)` continuations sit in the queue until the next time
   JSC happens to flush them — which is almost always "too late".
4. If you're using `TimerManager`, call `Tick()`. It drains microtasks
   internally after every fired callback.

## Microtasks

`Promise.resolve().then(fn)`, `await`, `queueMicrotask(fn)` — all of these
schedule a microtask. JSC runs microtasks at script-completion boundaries,
but only when something tells it to. Two ways to make that happen:

```cpp
ctx.DrainMicrotasks();   // explicit
```

…or implicitly: after `Value::Call`, after `Eval`, after `EvalModule`. The
library drains in those entry points already, but if your callback ran JS
internally you may want one more drain before yielding back to the host.

Microtask draining acquires the JSC API lock; do not call it from another
thread that already holds the lock.

## Timers — you must implement these

EmbeddedJSC deliberately does **not** define `setTimeout`, `setInterval`,
`clearTimeout`, `clearInterval`. The reason:

- A daemon polls a kqueue/epoll loop.
- A game engine ticks every frame.
- A CLI runs synchronously to completion.
- A worker thread sleeps on its own condvar.

Picking a scheduler for the embedder would either be wrong or constrain them.

We ship `ejsc::extra::TimerManager` as a **reference implementation** that
fits the per-frame pattern:

```cpp
#include <ejsc/extra/timer_manager.h>

ejsc::extra::TimerManager timers(ctx);
timers.Install();      // binds setTimeout/setInterval/clear*

// Per frame:
timers.Tick();         // fires due callbacks, drains microtasks
```

`TimerManager` is **not** auto-linked into `ejsc`. To use it, link
`ejsc::extra`:

```cmake
target_link_libraries(yourtarget PRIVATE ejsc::ejsc ejsc::extra)
```

If `TimerManager` doesn't fit (e.g. you want a libuv-style timerfd, or
sub-millisecond resolution, or off-thread firing), copy `timer_manager.cpp`
into your project and customise it. The class is ~150 lines.

See [examples/timers/main.cpp](examples/timers/main.cpp) for an end-to-end
driver.

## Exception handling

JS exceptions raised inside any `Eval`/`Call`/`EvalModule` are captured on
the Context, not thrown into C++:

```cpp
ctx.Eval("throw new Error('boom')", "frob.js");

if (ctx.HasException()) {
    ejsc::Value e = ctx.TakeException();    // clears the slot
    std::cerr << e.ToString().value_or("<unknown>") << '\n';
}
```

Conventions:

- The exception slot holds at most one value. A second exception overwrites
  the first; don't ignore it.
- `TakeException()` is the only way to clear the slot. If you don't take it,
  every subsequent call still reports `HasException() == true`.
- C++ exceptions thrown from a native function lambda are converted to JS
  `Error` objects automatically (see [Binding native functions](#binding-native-functions)).

## Threading and re-entrancy

- **All JSC operations are single-threaded.** A `Context` is bound to the
  thread it was created on; calling into it from another thread is
  undefined behaviour. The library does not guard against this — be
  explicit about your threading model.
- **Multiple contexts in one process is fine.** They share JSC's process-wide
  state but have isolated VMs.
- **Native callbacks can re-enter the engine.** A native function may call
  back into JS (`v.Call(...)`), evaluate another script, or fire more
  timers. The JSC API lock is reentrant within a single thread.
- **Native module registration is mutex-guarded** inside a Context, but
  treat it as a setup-time operation, not a hot-path one.

## What is intentionally not provided

- **Timers.** See [above](#timers--you-must-implement-these).
- **`console.log`.** Trivially layered in two lines by the embedder — the
  library doesn't pick a stream for you.
- **`globalThis`-style web APIs** (URL, fetch, TextEncoder, etc.). Add them
  as native functions or modules in your host.
- **Class-binding helpers.** For now, build classes by hand
  (`Value::Object(ctx)` + `SetProperty` + functions). A class-binding API
  is a v2 candidate.
- **TypedArray helpers.** Use JSC's C API directly through
  `Value::GetRef()` if you need raw access today.
- **Cross-platform builds.** Windows-only for the moment.
- **Coroutine/`co_await` integration.** Microtask draining is the
  primitive; build co_await on top if you need it.
