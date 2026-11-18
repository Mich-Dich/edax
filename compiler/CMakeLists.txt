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
set(RIFT_STD_MODULES std filesystem env build build/mode)
set(RIFT_STD_ENTRIES "")
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
    string(APPEND RIFT_STD_ENTRIES "    {\"${name}\", \"<std>/${module}.rifth\", R\"rift_std(${header_text})rift_std\",\n"
                                   "     \"<std>/${module}.rifts\", R\"rift_std(${source_text})rift_std\"},\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${header} ${source})
endforeach()
configure_file(src/std_library.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/std_library.cpp @ONLY)
target_compile_definitions(rift PRIVATE RIFT_VERSION="${PROJECT_VERSION}")

# LLVM (the backend). Only the backend touches LLVM headers; the front end does not.
# Asking for 18.1 (every LLVM 18 release) skips other LLVM installs on the search path (e.g. the Vulkan SDK ships
# DirectXShaderCompiler's LLVM fork with a CMake package that does not even load).
find_package(LLVM 18.1 REQUIRED CONFIG HINTS /usr/lib/llvm-18/lib/cmake/llvm /usr/lib/llvm-18)
if (NOT LLVM_VERSION_MAJOR EQUAL 18)
    message(FATAL_ERROR "riftc needs LLVM 18 (found ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR})")
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
