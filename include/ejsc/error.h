#pragma once

#include <stdexcept>
#include <string>

namespace ejsc {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace ejsc
