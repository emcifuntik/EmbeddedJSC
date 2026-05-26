#pragma once

#include "ejsc/value.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <JavaScriptCore/JSBase.h>

namespace ejsc {
class Context;
}

namespace ejsc::internal {

// Type-erased callable signatures. The template-side API (ejsc::ClassBuilder<T>)
// wraps the user's strongly-typed lambdas in adapters that cast through void*.
using ErasedConstructor = std::function<void*(Context&, std::span<const Value>)>;
using ErasedDestructor  = std::function<void(void*)>;
using ErasedMethod      = std::function<Value(void*, Context&, std::span<const Value>)>;

struct ClassData;

// Wrapper sitting in the instance's JSObject private slot. Lets the finalizer
// know whether to delete the C++ object and how (via classData->destructor).
struct InstanceData {
    void*       obj       = nullptr;   // T*, type-erased
    bool        owned     = false;     // true => owning JS handle (finalize deletes)
    ClassData*  classData = nullptr;
};

// Per-registered-class state. Owned by the Context (held in ContextState's
// `classes` vector); raw pointer is what Class<T> handles, instance private
// data, and the constructor function's private data all reference.
struct ClassData {
    ClassData() = default;
    ~ClassData();
    ClassData(const ClassData&) = delete;
    ClassData& operator=(const ClassData&) = delete;

    std::string name;
    Context*    ctx = nullptr;

    ErasedConstructor constructor;
    ErasedDestructor  destructor;

    // (method-name, erased-fn) pairs. Stored so we can keep the closures
    // alive — the Value::Function instances on the prototype reference them
    // by-copy, so this is redundant for lifetime but useful for introspection
    // / reflection later.
    std::vector<std::pair<std::string, ErasedMethod>> methods;

    JSClassRef instanceClass = nullptr;   // class for instance objects
    JSClassRef ctorClass     = nullptr;   // class for the constructor fn

    // Prototype object holding the method functions; shared by all instances.
    Value prototype;

    // The JS constructor function (callable with `new`). Embedders install
    // this as a global or module export.
    Value constructorValue;
};

// Builder-side helpers. Implemented in class_bridge.cpp.
ClassData* CreateClass(Context& ctx, std::string name,
                       ErasedConstructor ctor, ErasedDestructor dtor);
void ClassAddMethod(ClassData& cd, std::string methodName, ErasedMethod fn);
void ClassFinalize(ClassData& cd);   // builds prototype + constructor fn

// Runtime-side helpers, used by Class<T>::New / Wrap / Unwrap / IsInstance.
Value ClassNew(ClassData& cd, std::span<const Value> args);
Value ClassWrap(ClassData& cd, void* ptr);
void* ClassUnwrap(const ClassData& cd, const Value& v);
bool  ClassIsInstance(const ClassData& cd, const Value& v);
Value ClassConstructorValue(const ClassData& cd);

} // namespace ejsc::internal
