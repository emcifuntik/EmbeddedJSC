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
using ErasedGetter      = std::function<Value(void*, Context&)>;
using ErasedSetter      = std::function<void(void*, Context&, const Value&)>;
using ErasedCast        = std::function<void*(void*)>;  // derived T* -> Parent*

struct ClassData;

// Wrapper sitting in the instance's JSObject private slot. Lets the finalizer
// know whether to delete the C++ object and how (via classData->destructor).
struct InstanceData {
    void*       obj       = nullptr;   // T*, type-erased to the most-derived class
    bool        owned     = false;     // true => owning JS handle (finalize deletes)
    ClassData*  classData = nullptr;
};

// One accessor property registered on a class. Setter is empty for read-only.
struct PropertyEntry {
    std::string  name;
    ErasedGetter getter;
    ErasedSetter setter;
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

    // (method-name, erased-fn) pairs. Stored for introspection; the live
    // closures are also baked into the prototype's method functions.
    std::vector<std::pair<std::string, ErasedMethod>> methods;

    // Accessor properties. Dispatched by classGetPropertyDispatch /
    // classSetPropertyDispatch via name lookup; walks `parent` to find
    // inherited properties.
    std::vector<PropertyEntry> properties;

    // Inheritance. `parent` is non-null for a child class. `castToParent`
    // erases `static_cast<Parent*>(static_cast<T*>(p))`; used to walk up
    // the chain so an inherited method/accessor receives a correctly-typed
    // self pointer even when multiple inheritance introduces non-zero offsets.
    ClassData* parent = nullptr;
    ErasedCast castToParent;

    JSClassRef instanceClass = nullptr;   // class for instance objects
    JSClassRef ctorClass     = nullptr;   // class for the constructor fn

    // Prototype object holding the method functions; shared by all instances.
    // Its __proto__ is set to parent->prototype when inheriting.
    Value prototype;

    // The JS constructor function. Embedders install this as a global or
    // module export.
    Value constructorValue;
};

// Builder-side helpers. Implemented in class_bridge.cpp.
ClassData* CreateClass(Context& ctx, std::string name,
                       ErasedConstructor ctor, ErasedDestructor dtor);
void  ClassAddMethod(ClassData& cd, std::string methodName, ErasedMethod fn);
void  ClassAddProperty(ClassData& cd, std::string name,
                       ErasedGetter getter, ErasedSetter setter);
void  ClassSetParent(ClassData& cd, ClassData* parent, ErasedCast castToParent);
void  ClassFinalize(ClassData& cd);   // builds JSClasses, prototype, ctor

// Runtime-side helpers, used by Class<T>::New / Wrap / Unwrap / IsInstance.
Value ClassNew(ClassData& cd, std::span<const Value> args);
Value ClassWrap(ClassData& cd, void* ptr);
void* ClassUnwrap(const ClassData& cd, const Value& v);
bool  ClassIsInstance(const ClassData& cd, const Value& v);
Value ClassConstructorValue(const ClassData& cd);

// Walk up the parent chain from `mostDerived` to `target`, applying each
// ClassData's castToParent. Returns the appropriately-typed pointer.
// Returns nullptr if `target` is not in the chain (caller bug).
void* CastUpChain(ClassData* mostDerived, ClassData* target, void* ptr);

} // namespace ejsc::internal
