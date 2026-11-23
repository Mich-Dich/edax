#pragma once
// Large struct and array values in memory instead of LLVM SSA values (lower_aggregates.cpp).

#include <cstdint>

namespace llvm {
class Module;
}

namespace edax {

// Every value of a struct or array type of at least `limit_bytes` gets a slot in memory, and the
// operations on it become memcpys; Edax functions take and return such values through pointers.
void lower_large_aggregates(llvm::Module& m, std::uint64_t limit_bytes = 128);

} // namespace edax
