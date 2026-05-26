#pragma once

#include "fwd.h"
#include "context.h"

namespace ejsc {

class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    Context NewContext();
};

} // namespace ejsc
