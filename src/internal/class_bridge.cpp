#include "class_bridge.h"
#include "context_state.h"

#include "ejsc/context.h"
#include "ejsc/error.h"
#include "ejsc/value.h"

#include <vector>

#include <JavaScriptCore/JavaScript.h>

namespace ejsc::internal {

namespace {

// -- helpers ----------------------------------------------------------------

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

JSGlobalContextRef cref(const ClassData& cd) {
    return cd.ctx->State()->ctxRef;
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

// callAsFunction shim — JS code that calls `Foo(args)` without `new`. We
// route it through the same constructor path; the result is still a new
// owned instance.
JSValueRef constructorCallAsFunction(JSContextRef ctx, JSObjectRef function,
                                     JSObjectRef /*thisObject*/,
                                     size_t argc, const JSValueRef args[],
                                     JSValueRef* exception) {
    return constructorCallback(ctx, function, argc, args, exception);
}

// -- ClassData lifecycle ----------------------------------------------------

} // namespace

ClassData::~ClassData() {
    // prototype and constructorValue are Value handles; they release their
    // protected refs in their own destructors.
    if (instanceClass) JSClassRelease(instanceClass);
    if (ctorClass)     JSClassRelease(ctorClass);
}

ClassData* CreateClass(Context& ctx, std::string name,
                       ErasedConstructor ctor, ErasedDestructor dtor) {
    auto data = std::make_unique<ClassData>();
    data->name = std::move(name);
    data->ctx  = &ctx;
    data->constructor = std::move(ctor);
    data->destructor  = std::move(dtor);

    JSClassDefinition instDef = kJSClassDefinitionEmpty;
    instDef.className = data->name.c_str();   // JSC copies internally.
    instDef.finalize  = instanceFinalize;
    data->instanceClass = JSClassCreate(&instDef);

    JSClassDefinition ctorDef = kJSClassDefinitionEmpty;
    ctorDef.className          = data->name.c_str();
    ctorDef.callAsConstructor  = constructorCallback;
    ctorDef.callAsFunction     = constructorCallAsFunction;
    data->ctorClass = JSClassCreate(&ctorDef);

    ClassData* raw = data.get();
    ctx.State()->classes.push_back(std::move(data));
    return raw;
}

void ClassAddMethod(ClassData& cd, std::string methodName, ErasedMethod fn) {
    cd.methods.emplace_back(std::move(methodName), std::move(fn));
}

void ClassFinalize(ClassData& cd) {
    Context& ctx = *cd.ctx;

    // Build the shared prototype object and attach method-Functions to it.
    cd.prototype = Value::Object(ctx);

    for (auto& [name, erasedFn] : cd.methods) {
        // Capture the erased fn by value so the closure owns it.
        auto fn = erasedFn;
        std::string nameOwned = name;
        ClassData* classDataPtr = &cd;

        Value methodFn = Value::Function(ctx, name,
            [fn = std::move(fn), nameOwned = std::move(nameOwned), classDataPtr]
            (Context& c, const Value& thisVal, std::span<const Value> args) -> Value {
                JSGlobalContextRef cr = c.State()->ctxRef;
                if (!JSValueIsObjectOfClass(cr, thisVal.GetRef(), classDataPtr->instanceClass)) {
                    throw Error("ejsc: '" + nameOwned + "' called on non-" + classDataPtr->name + " object");
                }
                JSObjectRef thisObj = JSValueToObject(cr, thisVal.GetRef(), nullptr);
                auto* d = static_cast<InstanceData*>(JSObjectGetPrivate(thisObj));
                if (!d || !d->obj) {
                    throw Error("ejsc: '" + nameOwned + "' invoked on detached instance");
                }
                return fn(d->obj, c, args);
            });
        cd.prototype.SetProperty(name, methodFn);
    }

    // Build the constructor function (a JSObject of cd.ctorClass with ClassData
    // in private data).
    JSObjectRef ctorObj = JSObjectMake(cref(cd), cd.ctorClass, &cd);
    cd.constructorValue = Value::Adopt(ctx, ctorObj);

    // Wire prototype <-> constructor (JS convention):
    //   Foo.prototype = proto;
    //   proto.constructor = Foo;
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
    return d ? d->obj : nullptr;
}

bool ClassIsInstance(const ClassData& cd, const Value& v) {
    if (!v.IsObject()) return false;
    return JSValueIsObjectOfClass(cref(cd), v.GetRef(), cd.instanceClass);
}

Value ClassConstructorValue(const ClassData& cd) {
    return cd.constructorValue;
}

} // namespace ejsc::internal
