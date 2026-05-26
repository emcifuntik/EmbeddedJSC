#pragma once

namespace JSC {
struct GlobalObjectMethodTable;
}

namespace ejsc::internal {

// Returns the GlobalObjectMethodTable used for all ejsc-managed globals.
// Static lifetime; safe to take its address.
const JSC::GlobalObjectMethodTable& GetGlobalObjectMethodTable();

} // namespace ejsc::internal
