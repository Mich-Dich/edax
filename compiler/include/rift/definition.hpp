#pragma once
// Go to definition: the declaration of the name at a place in a file (`riftc definition`).

#include "rift/symbols.hpp"

#include <cstdint>
#include <optional>

namespace rift {

// Where the name at byte `offset` of `file` is declared, or none (not a name, or not resolved).
// An `import` goes to the module's header.
std::optional<Span> find_definition(const sema::Program& program, FileId file, std::uint32_t offset);

} // namespace rift
