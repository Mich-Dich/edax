cmake_minimum_required(VERSION 3.16)
project(mini VERSION 1.2.3 LANGUAGES C)

option(MINI_WITH_EXTRA "Build the extra part" OFF)
set(MINI_SCALE 2 CACHE STRING "The scale")
include(CheckIncludeFile)
check_include_file(stdint.h MINI_HAVE_STDINT)

set(SOURCES src/mini.c)
if(MINI_WITH_EXTRA AND NOT WIN32)
    add_subdirectory(extra)
    list(APPEND SOURCES ${EXTRA_SOURCES})
endif()

function(mini_add_defs target)
    foreach(d IN ITEMS ${ARGN})
        target_compile_definitions(${target} PRIVATE ${d})
    endforeach()
endfunction()

configure_file(mini_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/mini_config.h)
add_library(mini STATIC ${SOURCES})
add_library(mini::mini ALIAS mini)
target_include_directories(mini PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
math(EXPR MINI_DOUBLE "${MINI_SCALE} * 2")
mini_add_defs(mini MINI_DOUBLE=${MINI_DOUBLE} MINI_VERSION_MAJOR=${PROJECT_VERSION_MAJOR})
target_compile_features(mini PUBLIC c_std_11)
install(TARGETS mini)
