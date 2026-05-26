#pragma once

namespace ejsc::internal {

// Performs WTF + JSC one-time initialization. Thread-safe; idempotent.
void EnsureJSCInitialized();

} // namespace ejsc::internal
