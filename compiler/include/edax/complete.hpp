#pragma once
// Suggestions for editors (`edaxc complete`): the names that fit where the cursor is.

#include "edax/source.hpp"
#include "edax/symbols.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace edax {

// What is being typed at the cursor.
struct CompletionQuery {
    enum class Kind {
        none,     // nothing to suggest (a comment, a string, a number)
        path,     // `a::b::na|`: the items of `qualifier`
        member,   // `x.f().na|`: the fields and methods of what `receiver` names
        name,     // `na|`: the names in scope
    };
    Kind                     kind = Kind::none;
    std::string              prefix;      // the part of the name before the cursor
    std::vector<std::string> qualifier;   // path: the segments before the last `::`
    std::vector<std::string> receiver;    // member: the steps before the last `.` (`f()`, `xs[]`, `a`)
    std::uint32_t            start = 0;   // where the whole expression being typed starts
    std::uint32_t            end   = 0;   // where the name under the cursor ends
};

// One suggestion: `kind` is "function", "type", "field", "namespace", "keyword", ...;
// `detail` the declaration as written, `doc` its comment.
struct Completion {
    std::string name;
    std::string kind;
    std::string detail;
    std::string doc;
};

// What is being typed at byte `offset` of `text`.
CompletionQuery completion_query(std::string_view text, std::uint32_t offset);

// `text` with the expression being typed blanked out (spaces; offsets stay), so the rest of the
// file parses; with `whole_line` the whole line around it.
std::string without_query(std::string_view text, const CompletionQuery& query, std::uint32_t offset, bool whole_line);

// The suggestions for `query` at `offset` of `file`. `program` is the checked program of the file
// (null when it didn't load), `library` a program importing every std module, for `std::` names.
std::vector<Completion> find_completions(const sema::Program* program, const SourceManager& sources, const sema::Program& library,
                                         const SourceManager& library_sources, FileId file, std::uint32_t offset,
                                         const CompletionQuery& query);

} // namespace edax
