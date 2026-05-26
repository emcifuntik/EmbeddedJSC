#include "ejsc/runtime.h"
#include "internal/jsc_init.h"

namespace ejsc {

Runtime::Runtime() {
    internal::EnsureJSCInitialized();
}

Runtime::~Runtime() = default;

Context Runtime::NewContext() {
    return Context();
}

} // namespace ejsc
