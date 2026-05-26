#pragma once

#include "ejsc/value.h"

#include <string>
#include <unordered_map>

namespace ejsc::internal {

// Holds the resolved exports of a native module, ready to be materialized as a
// JSC module namespace object when the module is imported.
struct NativeModuleEntry {
    std::string name;

    // Each entry is held as a Value (which keeps a JSC reference protected).
    // For functions, this is a JSObjectRef wrapped as Value.
    // For ordinary values, this is whatever the embedder produced.
    std::unordered_map<std::string, Value> exports;
};

} // namespace ejsc::internal
