# --- unit tests (C++, in-process) -------------------------------------------------------------
# Every tests/unit/test_*.cpp is picked up automatically.
file(GLOB EDAX_UNIT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_*.cpp)

add_executable(edax_unit_tests
    unit/framework/edax_test_main.cpp
    ${EDAX_UNIT_TEST_SOURCES}
)
target_include_directories(edax_unit_tests PRIVATE unit/framework)
target_link_libraries(edax_unit_tests PRIVATE edax)

add_test(NAME unit COMMAND edax_unit_tests)

# --- feature + action tests (Python runner driving edaxc) --------------------------------------
find_package(Python3 3.10 COMPONENTS Interpreter)
if (Python3_FOUND)
    foreach (suite feature action)
        add_test(
            NAME ${suite}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.py
                    --suite ${suite} --color never
                    --edaxc $<TARGET_FILE:edaxc>
        )
    endforeach()
else()
    message(WARNING "Python 3.10+ not found: feature and action tests are not registered with ctest")
endif()
