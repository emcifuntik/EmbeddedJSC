#pragma once

#include "fwd.h"
#include "value.h"

#include <memory>
#include <string>
#include <string_view>

namespace ejsc {

namespace internal { struct NativeModuleEntry; }

class ModuleBuilder {
public:
    ModuleBuilder& Export(std::string_view name, Value v);
    ModuleBuilder& ExportFunction(std::string_view name, Value::NativeFn fn);
    void Build();

    // Internal: constructed by Context::NewModule.
    ModuleBuilder(Context& ctx, std::string name);
    ~ModuleBuilder();
    ModuleBuilder(ModuleBuilder&&) noexcept;
    ModuleBuilder& operator=(ModuleBuilder&&) noexcept;
    ModuleBuilder(const ModuleBuilder&) = delete;
    ModuleBuilder& operator=(const ModuleBuilder&) = delete;

private:
    Context* m_ctx;
    std::string m_name;
    std::unique_ptr<internal::NativeModuleEntry> m_entry;
    bool m_built = false;
};

} // namespace ejsc
