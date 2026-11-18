# librift: the compiler as a library (everything except main). Unit tests link against it.
add_library(rift STATIC
    src/source.cpp
    src/diagnostics.cpp
    src/token.cpp
    src/lexer.cpp
    src/parser.cpp
    src/ast_printer.cpp
    src/ast_clone.cpp
    src/modules.cpp
    src/resolve.cpp
    src/types.cpp
    src/check.cpp
    src/compile_time.cpp
    src/ownership.cpp
    src/llvm_backend.cpp
    src/llvm_runtime.cpp
    src/driver.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/std_library.cpp
)
target_include_directories(rift PUBLIC include)

# The standard library's Rift source is compiled into riftc. CMake re-runs when it changes.
# Each module is a .rifth and a .rifts in library/std: `std` is std.*, `std::filesystem` is
# filesystem.*. The first one is the core module.
set(RIFT_STD_MODULES std filesystem env build build/mode thread time)
set(RIFT_STD_ENTRIES "")
set(RIFT_STD_TEXTS "")
# A text becomes a function that joins pieces of at most 60000 characters at startup: C++ only
# promises string literals up to 65536 bytes (clang's -Woverlength-strings).
set(rift_text_index 0)
function(rift_std_text text out)
    math(EXPR index "${rift_text_index} + 1")
    set(rift_text_index ${index} PARENT_SCOPE)
    string(LENGTH "${text}" length)
    set(pieces "")
    set(at 0)
    while (at LESS length)
        string(SUBSTRING "${text}" ${at} 60000 piece)
        string(APPEND pieces "        R\"rift_std(${piece})rift_std\",\n")
        math(EXPR at "${at} + 60000")
    endwhile()
    set(RIFT_STD_TEXTS "${RIFT_STD_TEXTS}const std::string& std_text_${index}() {\n    static const std::string text = join({\n${pieces}    });\n    return text;\n}\n\n" PARENT_SCOPE)
    set(${out} "std_text_${index}()" PARENT_SCOPE)
endfunction()
foreach(module ${RIFT_STD_MODULES})
    set(header ${PROJECT_SOURCE_DIR}/library/std/${module}.rifth)
    set(source ${PROJECT_SOURCE_DIR}/library/std/${module}.rifts)
    file(READ ${header} header_text)
    file(READ ${source} source_text)
    if (module STREQUAL "std")
        set(name "std")
    else()
        string(REPLACE "/" "::" name "std::${module}")
    endif()
    rift_std_text("${header_text}" header_call)
    rift_std_text("${source_text}" source_call)
    string(APPEND RIFT_STD_ENTRIES "    {\"${name}\", \"<std>/${module}.rifth\", ${header_call},\n"
                                   "     \"<std>/${module}.rifts\", ${source_call}},\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${header} ${source})
endforeach()
configure_file(src/std_library.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/std_library.cpp @ONLY)
target_compile_definitions(rift PRIVATE RIFT_VERSION="${PROJECT_VERSION}" RIFT_STD_DIR="${PROJECT_SOURCE_DIR}/library/std")

# LLVM (the backend). Only the backend touches LLVM headers; the front end does not.
# riftc builds with LLVM 18 to 21. Each major version is asked for by number, which skips other
# LLVM installs on the search path (e.g. the Vulkan SDK ships DirectXShaderCompiler's LLVM fork
# with a CMake package that does not even load). Tested versions first; pass
# -DRIFT_LLVM_VERSION=21 (or -DLLVM_DIR=...) to pick one.
set(RIFT_LLVM_VERSION "" CACHE STRING "LLVM major version to use (18-21); empty: the first one found of 18, 20, 21, 19")
if (RIFT_LLVM_VERSION)
    set(rift_llvm_candidates ${RIFT_LLVM_VERSION})
else()
    set(rift_llvm_candidates 18 20 21 19)
endif()
if (LLVM_DIR)   # given explicitly
    find_package(LLVM REQUIRED CONFIG)
else()
    foreach(version ${rift_llvm_candidates})
        find_package(LLVM QUIET CONFIG NO_DEFAULT_PATH
                     PATHS /usr/lib/llvm-${version}/lib/cmake/llvm /usr/lib/llvm-${version}/cmake
                           /usr/lib64/llvm${version}/lib/cmake/llvm /opt/homebrew/opt/llvm@${version}/lib/cmake/llvm
                           /usr/local/opt/llvm@${version}/lib/cmake/llvm)
        if (LLVM_FOUND AND LLVM_VERSION_MAJOR EQUAL version)
            break()
        endif()
        unset(LLVM_FOUND)
        unset(LLVM_DIR CACHE)
    endforeach()
endif()
if (NOT LLVM_FOUND OR LLVM_VERSION_MAJOR LESS 18 OR LLVM_VERSION_MAJOR GREATER 21)
    message(FATAL_ERROR "riftc needs LLVM 18, 19, 20 or 21 (development files). On Debian/Ubuntu: "
                        "sudo apt install llvm-20-dev libzstd-dev (or llvm-18-dev), or set LLVM_DIR")
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
llvm_map_components_to_libnames(RIFT_LLVM_LIBS
    core support irreader linker target analysis passes codegen native)
target_include_directories(rift SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
target_link_libraries(rift PRIVATE ${RIFT_LLVM_LIBS})

# riftc: the command-line driver.
add_executable(riftc driver/main.cpp)
target_link_libraries(riftc PRIVATE rift)
