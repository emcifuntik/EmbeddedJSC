#include "synthetic_module.h"

// Currently a header-only descriptor. The actual JSC plumbing that turns a
// NativeModuleEntry into a module namespace lives in module_loader.cpp, where
// the JSC module-loader callbacks have direct access to the VM.
//
// Kept as a separate TU so future internals (custom AbstractModuleRecord
// subclass, etc.) have a clear home.

namespace ejsc::internal {
} // namespace ejsc::internal
