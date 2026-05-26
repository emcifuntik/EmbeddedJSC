#include "class_bridge.h"
#include "context_state.h"

#include "ejsc/context.h"
#include "ejsc/error.h"
#include "ejsc/value.h"

#include <string>
#include <vector>

#include <JavaScriptCore/JavaScript.h>

namespace ejsc::internal {

namespace {

// -- small helpers ----------------------------------------------------------

class JSStringHolder {
public:
    explicit JSStringHolder(const char* s) : m_str(JSStringCreateWithUTF8CString(s)) {}
    explicit JSStringHolder(const std::string& s) : m_str(JSStringCreateWithUTF8CString(s.c_str())) {}
    ~JSStringHolder() { if (m_str) JSStringRelease(m_str); }
    JSStringHolder(const JSStringHolder&) = delete;
    JSStringHolder& operator=(const JSStringHolder&) = delete;
    operator JSStringRef() const { return m_str; }
private:
    JSStringRef m_str;
};

void setExceptionString(JSContextRef ctx, JSValueRef* exception, const char* msg) {
    if (!exception) return;
    JSStringHolder s(msg);
    JSValueRef args[] = { JSValueMakeString(ctx, s) };
    *exception = JSObjectMakeError(ctx, 1, args, nullptr);
}

std::string utf8FromJSString(JSStringRef s) {
    size_t bufSize = JSStringGetMaximumUTF8CStringSize(s);
    std::vector<char> buf(bufSize);
    JSStringGetUTF8CString(s, buf.data(), bufSize);
    return std::string(buf.data());
}

JSGlobalContextRef cref(const ClassData& cd) {
    return cd.ctx->State()->ctxRef;
}

// Walk `mostDerived`'s parent chain looking for `name`. Returns the entry
// and writes the owning class to `owner`.
const PropertyEntry* findProperty(ClassData* mostDerived,
                                  const std::string& name,
                                  ClassData** owner) {
    for (ClassData* c = mostDerived; c; c = c->parent) {
        for (const auto& p : c->properties) {
            if (p.name == name) {
                if (owner) *owner = c;
                return &p;
            }
        }
    }
    return nullptr;
}

// -- finalize & construct callbacks ----------------------------------------

void instanceFinalize(JSObjectRef obj) {
    auto* d = static_cast<InstanceData*>(JSObjectGetPrivate(obj));
    if (!d) return;
    if (d->owned && d->obj && d->classData && d->classData->destructor) {
        d->classData->destructor(d->obj);
    }
    delete d;
    JSObjectSetPrivate(obj, nullptr);
}

JSObjectRef constructorCallback(JSContextRef ctx, JSObjectRef ctorObj,
                                size_t argc, const JSValueRef args[],
                                JSValueRef* exception) {
    auto* cd = static_cast<ClassData*>(JSObjectGetPrivate(ctorObj));
    if (!cd) {
        setExceptionString(ctx, exception, "ejsc: ClassData missing on constructor");
        return JSObjectMake(ctx, nullptr, nullptr);
    }
    if (!cd->constructor) {
        setExceptionString(ctx, exception, "ejsc: class has no Constructor()");
        return JSObjectMake(ctx, nullptr, nullptr);
    }

    std::vector<Value> argValues;
    argValues.reserve(argc);
    for (size_t i = 0; i < argc; ++i) {
        argValues.push_back(Value::Adopt(*cd->ctx, args[i]));
    }

    void* obj = nullptr;
    try {
        obj = cd->constructor(*cd->ctx,
                              std::span<const Value>(argValues.data(), argValues.size()));
    } catch (const std::exception& e) {
        setExceptionString(ctx, exception, e.what());
        return JSObjectMake(ctx, nullptr, nullptr);
    } catch (...) {
        setExceptionString(ctx, exception, "ejsc: constructor threw");
        return JSObjectMake(ctx, nullptr, nullptr);
    }

    if (!obj) {
        setExceptionString(ctx, exception, "ejsc: constructor returned null");
        return JSObjectMake(ctx, nullptr, nullptr);
    }

    auto* data = new InstanceData{obj, /*owned=*/true, cd};
    JSObjectRef instance = JSObjectMake(ctx, cd->instanceClass, data);
    JSObjectSetPrototype(ctx, instance, cd->prototype.GetRef());
    return instance;
}

// callAsFunction shim — JS code that calls `Foo(args)` without `new` is
// routed through the constructor path so the result is still a new owned
// instance.
JSValueRef constructorCallAsFunction(JSContextRef ctx, JSObjectRef function,
                                     JSObjectRef /*thisObject*/,
                                     size_t argc, const JSValueRef args[],
                                     JSValueRef* exception) {
    return constructorCallback(ctx, function, argc, args, exception);
}

// hasInstance for `obj instanceof Foo`. JSCallbackObject returns false by
// default unless this is provided, so we re-implement the standard
// OrdinaryHasInstance walk: get `constructor.prototype`, then check whether
// it appears anywhere in `value`'s prototype chain.
bool constructorHasInstance(JSContextRef ctx, JSObjectRef constructor,
                            JSValueRef possibleInstance, JSValueRef* /*exception*/) {
    if (!JSValueIsObject(ctx, possibleInstance)) return false;

    JSStringHolder protoName("prototype");
    JSValueRef protoVal = JSObjectGetProperty(ctx, constructor, protoName, nullptr);
    if (!protoVal || !JSValueIsObject(ctx, protoVal)) return false;

    JSObjectRef instanceObj = JSValueToObject(ctx, possibleInstance, nullptr);
    if (!instanceObj) return false;

    JSValueRef proto = JSObjectGetPrototype(ctx, instanceObj);
    while (proto && !JSValueIsNull(ctx, proto)) {
        if (JSValueIsStrictEqual(ctx, proto, protoVal)) return true;
        JSObjectRef protoObj = JSValueToObject(ctx, proto, nullptr);
        if (!protoObj) break;
        proto = JSObjectGetPrototype(ctx, protoObj);
    }
    return false;
}

// -- accessor property dispatchers -----------------------------------------

// Class-level getProperty: called for every property access on an instance
// (before staticValues, before prototype chain lookup). Returning nullptr
// tells JSC to continue normal lookup, which is what lets methods on the
// prototype still resolve.
JSValueRef classGetPropertyDispatch(JSContextRef ctx, JSObjectRef object,
                                    JSStringRef propertyName,
                                    JSValueRef* exception) {
    auto* d = static_cast<InstanceData*>(JSObjectGetPrivate(object));
    if (!d || !d->classData) return nullptr;

    std::string name = utf8FromJSString(propertyName);
    ClassData* owner = nullptr;
    const PropertyEntry* p = findProperty(d->classData, name, &owner);
    if (!p) return nullptr;  // not an accessor — let JSC fall through

    void* casted = CastUpChain(d->classData, owner, d->obj);
    if (!casted) return JSValueMakeUndefined(ctx);

    try {
        Value v = p->getter(casted, *d->classData->ctx);
        return v.GetRef();
    } catch (const std::exception& e) {
        setExceptionString(ctx, exception, e.what());
        return JSValueMakeUndefined(ctx);
    }
}

// Class-level setProperty. Returning true means "handled"; false means "not
// my property, fall through to default behaviour" (which for our instance
// class means a TypeError on a class that doesn't accept dynamic properties,
// or silently setting on the prototype — whichever JSC does).
bool classSetPropertyDispatch(JSContextRef ctx, JSObjectRef object,
                              JSStringRef propertyName, JSValueRef value,
                              JSValueRef* exception) {
    auto* d = static_cast<InstanceData*>(JSObjectGetPrivate(object));
    if (!d || !d->classData) return false;

    std::string name = utf8FromJSString(propertyName);
    ClassData* owner = nullptr;
    const PropertyEntry* p = findProperty(d->classData, name, &owner);
    if (!p) return false;

    if (!p->setter) {
        setExceptionString(ctx, exception,
            ("ejsc: read-only property '" + name + "'").c_str());
        return true;  // handled (by throwing) — don't fall through
    }

    void* casted = CastUpChain(d->classData, owner, d->obj);
    if (!casted) return false;

    try {
        p->setter(casted, *d->classData->ctx,
                  Value::Adopt(*d->classData->ctx, value));
        return true;
    } catch (const std::exception& e) {
        setExceptionString(ctx, exception, e.what());
        return true;
    }
}

} // namespace

// -- ClassData lifecycle ----------------------------------------------------

ClassData::~ClassData() {
    if (instanceClass) JSClassRelease(instanceClass);
    if (ctorClass)     JSClassRelease(ctorClass);
}

void* CastUpChain(ClassData* mostDerived, ClassData* target, void* ptr) {
    for (ClassData* c = mostDerived; c != target; c = c->parent) {
        if (!c || !c->castToParent) return nullptr;
        ptr = c->castToParent(ptr);
    }
    return ptr;
}

ClassData* CreateClass(Context& ctx, std::string name,
                       ErasedConstructor ctor, ErasedDestructor dtor) {
    auto data = std::make_unique<ClassData>();
    data->name = std::move(name);
    data->ctx  = &ctx;
    data->constructor = std::move(ctor);
    data->destructor  = std::move(dtor);

    ClassData* raw = data.get();
    ctx.State()->classes.push_back(std::move(data));
    return raw;
}

void ClassAddMethod(ClassData& cd, std::string methodName, ErasedMethod fn) {
    cd.methods.emplace_back(std::move(methodName), std::move(fn));
}

void ClassAddProperty(ClassData& cd, std::string name,
                      ErasedGetter getter, ErasedSetter setter) {
    cd.properties.push_back(PropertyEntry{
        std::move(name), std::move(getter), std::move(setter)
    });
}

void ClassSetParent(ClassData& cd, ClassData* parent, ErasedCast castToParent) {
    cd.parent = parent;
    cd.castToParent = std::move(castToParent);
}

void ClassFinalize(ClassData& cd) {
    Context& ctx = *cd.ctx;

    // Build the instance JSClass.
    JSClassDefinition instDef = kJSClassDefinitionEmpty;
    instDef.className   = cd.name.c_str();   // copied by JSClassCreate
    instDef.finalize    = instanceFinalize;
    instDef.getProperty = classGetPropertyDispatch;
    instDef.setProperty = classSetPropertyDispatch;
    if (cd.parent) {
        instDef.parentClass = cd.parent->instanceClass;
    }
    cd.instanceClass = JSClassCreate(&instDef);

    // Build the constructor function's JSClass.
    JSClassDefinition ctorDef = kJSClassDefinitionEmpty;
    ctorDef.className         = cd.name.c_str();
    ctorDef.callAsConstructor = constructorCallback;
    ctorDef.callAsFunction    = constructorCallAsFunction;
    ctorDef.hasInstance       = constructorHasInstance;
    cd.ctorClass = JSClassCreate(&ctorDef);

    // Build the shared prototype.
    cd.prototype = Value::Object(ctx);

    // Inheritance: chain the prototype to the parent's prototype.
    if (cd.parent) {
        JSObjectRef proto = JSValueToObject(cref(cd), cd.prototype.GetRef(), nullptr);
        JSObjectSetPrototype(cref(cd), proto, cd.parent->prototype.GetRef());
    }

    // Attach methods to the prototype. Each method's closure captures the
    // ClassData that defined it so we can cast `this` up the chain.
    for (auto& [name, erasedFn] : cd.methods) {
        ClassData* ownerCd = &cd;
        auto fn = erasedFn;
        std::string nameOwned = name;

        Value methodFn = Value::Function(ctx, name,
            [fn = std::move(fn), nameOwned = std::move(nameOwned), ownerCd]
            (Context& c, const Value& thisVal, std::span<const Value> args) -> Value {
                JSGlobalContextRef cr = c.State()->ctxRef;
                if (!JSValueIsObjectOfClass(cr, thisVal.GetRef(), ownerCd->instanceClass)) {
                    throw Error("ejsc: '" + nameOwned + "' called on non-"
                                + ownerCd->name + " object");
                }
                JSObjectRef thisObj = JSValueToObject(cr, thisVal.GetRef(), nullptr);
                auto* d = static_cast<InstanceData*>(JSObjectGetPrivate(thisObj));
                if (!d || !d->obj) {
                    throw Error("ejsc: '" + nameOwned + "' invoked on detached instance");
                }
                void* casted = CastUpChain(d->classData, ownerCd, d->obj);
                if (!casted) {
                    throw Error("ejsc: cast chain broken for method '" + nameOwned + "'");
                }
                return fn(casted, c, args);
            });
        cd.prototype.SetProperty(name, methodFn);
    }

    // Constructor function value, with ClassData in private data.
    JSObjectRef ctorObj = JSObjectMake(cref(cd), cd.ctorClass, &cd);
    cd.constructorValue = Value::Adopt(ctx, ctorObj);

    // JS convention: Foo.prototype = proto; proto.constructor = Foo.
    cd.constructorValue.SetProperty("prototype", cd.prototype);
    cd.prototype.SetProperty("constructor", cd.constructorValue);
}

Value ClassNew(ClassData& cd, std::span<const Value> args) {
    if (!cd.constructor) {
        throw Error("ejsc: Class<" + cd.name + "> has no Constructor()");
    }

    void* obj = cd.constructor(*cd.ctx, args);
    if (!obj) {
        throw Error("ejsc: Class<" + cd.name + ">::New constructor returned null");
    }

    auto* data = new InstanceData{obj, /*owned=*/true, &cd};
    JSObjectRef instance = JSObjectMake(cref(cd), cd.instanceClass, data);
    JSObjectSetPrototype(cref(cd), instance, cd.prototype.GetRef());
    return Value::Adopt(*cd.ctx, instance);
}

Value ClassWrap(ClassData& cd, void* ptr) {
    auto* data = new InstanceData{ptr, /*owned=*/false, &cd};
    JSObjectRef instance = JSObjectMake(cref(cd), cd.instanceClass, data);
    JSObjectSetPrototype(cref(cd), instance, cd.prototype.GetRef());
    return Value::Adopt(*cd.ctx, instance);
}

void* ClassUnwrap(const ClassData& cd, const Value& v) {
    if (!v.IsObject()) return nullptr;
    if (!JSValueIsObjectOfClass(cref(cd), v.GetRef(), cd.instanceClass)) return nullptr;
    JSObjectRef obj = JSValueToObject(cref(cd), v.GetRef(), nullptr);
    auto* d = static_cast<InstanceData*>(JSObjectGetPrivate(obj));
    if (!d || !d->obj) return nullptr;

    // The instance was created against d->classData (the most-derived class).
    // Cast up to `cd` so the returned pointer is correctly-typed.
    return CastUpChain(d->classData, const_cast<ClassData*>(&cd), d->obj);
}

bool ClassIsInstance(const ClassData& cd, const Value& v) {
    if (!v.IsObject()) return false;
    return JSValueIsObjectOfClass(cref(cd), v.GetRef(), cd.instanceClass);
}

Value ClassConstructorValue(const ClassData& cd) {
    return cd.constructorValue;
}

} // namespace ejsc::internal
