# libedax: the compiler as a library (everything except main). Unit tests link against it.
add_library(edax STATIC
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
    src/lower_aggregates.cpp
    src/thin_lto.cpp
    src/driver.cpp
    src/c_import.cpp
    src/cpp_wrap.cpp
    src/cmake_reader.cpp
    src/definition.cpp
    src/graphics_registry.cpp
    src/build_report.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/std_library.cpp
)
target_include_directories(edax PUBLIC include)

# The standard library's Edax source is compiled into edaxc. CMake re-runs when it changes.
# Each module is a .edh and a .ed in library/std: `std` is std.*, `std::filesystem` is
# filesystem.*. The first one is the core module.
set(EDAX_STD_MODULES std filesystem env build build/mode thread time meta derive log parallel
    graphics windowing/core windowing/bindings windowing/x11 windowing
    math image text/font ui ui/opengl ui/immediate ui/platform ui/routing ui/plot ui/debug ui/dock ui/nodes)
set(EDAX_STD_ENTRIES "")
set(EDAX_STD_TEXTS "")
# A text becomes a function that joins pieces of at most 60000 characters at startup: C++ only
# promises string literals up to 65536 bytes (clang's -Woverlength-strings).
set(edax_text_index 0)
function(edax_std_text text out)
    math(EXPR index "${edax_text_index} + 1")
    set(edax_text_index ${index} PARENT_SCOPE)
    string(LENGTH "${text}" length)
    set(pieces "")
    set(at 0)
    while (at LESS length)
        string(SUBSTRING "${text}" ${at} 60000 piece)
        string(APPEND pieces "        R\"edax_std(${piece})edax_std\",\n")
        math(EXPR at "${at} + 60000")
    endwhile()
    set(EDAX_STD_TEXTS "${EDAX_STD_TEXTS}const std::string& std_text_${index}() {\n    static const std::string text = join({\n${pieces}    });\n    return text;\n}\n\n" PARENT_SCOPE)
    set(${out} "std_text_${index}()" PARENT_SCOPE)
endfunction()
foreach(module ${EDAX_STD_MODULES})
    set(header ${PROJECT_SOURCE_DIR}/library/std/${module}.edh)
    set(source ${PROJECT_SOURCE_DIR}/library/std/${module}.ed)
    file(READ ${header} header_text)
    file(READ ${source} source_text)
    if (module STREQUAL "std")
        set(name "std")
    else()
        string(REPLACE "/" "::" name "std::${module}")
    endif()
    edax_std_text("${header_text}" header_call)
    edax_std_text("${source_text}" source_call)
    string(APPEND EDAX_STD_ENTRIES "    {\"${name}\", \"<std>/${module}.edh\", ${header_call},\n"
                                   "     \"<std>/${module}.ed\", ${source_call}},\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${header} ${source})
endforeach()
# The Khronos registries std::graphics::opengl and std::graphics::vulkan are made from
# (compiler/src/graphics_registry.cpp), embedded like the library's text.
set(EDAX_STD_REGISTRIES "")
foreach(registry gl.xml vk.xml)
    set(file ${PROJECT_SOURCE_DIR}/library/std/graphics/registry/${registry})
    file(READ ${file} registry_text)
    edax_std_text("${registry_text}" registry_call)
    string(APPEND EDAX_STD_REGISTRIES "    if (file == \"${registry}\") return ${registry_call};\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${file})
endforeach()
# Binary files of the library (fonts), embedded as byte arrays: std::embed in a std module reads
# them from edaxc (check.cpp, std_resource).
set(EDAX_STD_RESOURCES text/fonts/DMSans-Regular.ttf text/fonts/DMSans-SemiBold.ttf text/fonts/DejaVuSansMono.ttf
    ui/shader.vert ui/shader.frag)
set(EDAX_STD_RESOURCE_DATA "")
set(EDAX_STD_RESOURCE_LOOKUP "")
set(edax_resource_index 0)
foreach(resource ${EDAX_STD_RESOURCES})
    set(file ${PROJECT_SOURCE_DIR}/library/std/${resource})
    file(READ ${file} resource_hex HEX)
    file(SIZE ${file} resource_size)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," resource_bytes "${resource_hex}")
    string(REGEX REPLACE "((0x..,){32})" "\\1\n    " resource_bytes "${resource_bytes}")
    string(APPEND EDAX_STD_RESOURCE_DATA "const unsigned char std_resource_${edax_resource_index}[] = {\n    ${resource_bytes}\n};\n\n")
    string(APPEND EDAX_STD_RESOURCE_LOOKUP "    if (path == \"${resource}\") return {reinterpret_cast<const char*>(std_resource_${edax_resource_index}), ${resource_size}};\n")
    math(EXPR edax_resource_index "${edax_resource_index} + 1")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${file})
endforeach()
configure_file(src/std_library.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/std_library.cpp @ONLY)
target_compile_definitions(edax PRIVATE EDAX_VERSION="${PROJECT_VERSION}" EDAX_STD_DIR="${PROJECT_SOURCE_DIR}/library/std")

# LLVM (the backend). Only the backend touches LLVM headers; the front end does not.
# edaxc builds with LLVM 18 to 21. Each major version is asked for by number, which skips other
# LLVM installs on the search path (e.g. the Vulkan SDK ships DirectXShaderCompiler's LLVM fork
# with a CMake package that does not even load). Tested versions first; pass
# -DEDAX_LLVM_VERSION=21 (or -DLLVM_DIR=...) to pick one.
set(EDAX_LLVM_VERSION "" CACHE STRING "LLVM major version to use (18-21); empty: the first one found of 18, 20, 21, 19")
if (EDAX_LLVM_VERSION)
    set(edax_llvm_candidates ${EDAX_LLVM_VERSION})
else()
    set(edax_llvm_candidates 18 20 21 19)
endif()
if (LLVM_DIR)   # given explicitly
    find_package(LLVM REQUIRED CONFIG)
else()
    foreach(version ${edax_llvm_candidates})
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
    message(FATAL_ERROR "edaxc needs LLVM 18, 19, 20 or 21 (development files). On Debian/Ubuntu: "
                        "sudo apt install llvm-20-dev libzstd-dev (or llvm-18-dev), or set LLVM_DIR")
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
llvm_map_components_to_libnames(EDAX_LLVM_LIBS
    core support irreader linker target analysis passes codegen native bitwriter bitreader ipo objcarcopts object
    aarch64codegen aarch64desc aarch64info aarch64asmparser
    x86codegen x86desc x86info x86asmparser
    riscvcodegen riscvdesc riscvinfo riscvasmparser)
target_include_directories(edax SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
# LLVM's ThinLTO (optimized builds through the object cache). Linked by file: the LTO component's
# CMake target drags in Polly, which Debian/Ubuntu's LLVM packages don't ship by default.
find_library(EDAX_LLVM_LTO NAMES LLVMLTO PATHS ${LLVM_LIBRARY_DIR} NO_DEFAULT_PATH)
find_library(EDAX_LLVM_EXTENSIONS NAMES LLVMExtensions PATHS ${LLVM_LIBRARY_DIR} NO_DEFAULT_PATH)
if(NOT EDAX_LLVM_LTO)
    message(FATAL_ERROR "edaxc needs LLVM's LTO library (libLLVMLTO) from ${LLVM_LIBRARY_DIR}")
endif()
# LLVM's LTO headers don't compile as C++26.
set_source_files_properties(src/thin_lto.cpp PROPERTIES COMPILE_OPTIONS "-std=c++20;-Wno-null-dereference")
# GCC's -Wnull-dereference fires inside LLVM's inlined ilist/CallBase code (false positives that
# -isystem can't silence once inlined). Keep it for every other source file.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_property(SOURCE src/llvm_backend.cpp src/lower_aggregates.cpp APPEND PROPERTY COMPILE_OPTIONS -Wno-null-dereference)
endif()
target_link_libraries(edax PRIVATE ${EDAX_LLVM_LTO} ${EDAX_LLVM_EXTENSIONS} ${EDAX_LLVM_LIBS})

# edaxc: the command-line driver.
add_executable(edaxc driver/main.cpp)
target_link_libraries(edaxc PRIVATE edax)
