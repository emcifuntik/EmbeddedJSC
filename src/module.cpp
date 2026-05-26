#include "ejsc/module.h"
#include "ejsc/context.h"
#include "ejsc/error.h"

#include "internal/context_state.h"
#include "internal/synthetic_module.h"

namespace ejsc {

ModuleBuilder::ModuleBuilder(Context& ctx, std::string name)
    : m_ctx(&ctx), m_name(std::move(name)),
      m_entry(std::make_unique<internal::NativeModuleEntry>()) {
    m_entry->name = m_name;
}

ModuleBuilder::~ModuleBuilder() = default;
ModuleBuilder::ModuleBuilder(ModuleBuilder&&) noexcept = default;
ModuleBuilder& ModuleBuilder::operator=(ModuleBuilder&&) noexcept = default;

ModuleBuilder& ModuleBuilder::Export(std::string_view name, Value v) {
    if (m_built) {
        throw Error("ejsc: ModuleBuilder already Built");
    }
    m_entry->exports.emplace(std::string(name), std::move(v));
    return *this;
}

ModuleBuilder& ModuleBuilder::ExportFunction(std::string_view name, Value::NativeFn fn) {
    return Export(name, Value::Function(*m_ctx, name, std::move(fn)));
}

void ModuleBuilder::Build() {
    if (m_built) return;
    auto* s = m_ctx->State();
    {
        std::lock_guard<std::mutex> lock(s->modulesMutex);
        s->nativeModules[m_name] = std::move(m_entry);
    }
    m_built = true;
    // TODO(spike, step 6): notify/prepare the JSC module loader so import 'name'
    // actually materializes a synthetic module from this entry. Today the
    // entry is stored but the loader's fetch hook rejects with
    // "native module fetch unimplemented".
}

} // namespace ejsc
