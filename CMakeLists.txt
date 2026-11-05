cmake_minimum_required(VERSION 4.4)

project(foolc
        VERSION 0.1.0
        LANGUAGES CXX
        DESCRIPTION "The Fool Systems Programming Language Compiler Frontend"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler Binary
add_executable(foolc
        src/main.cpp
        src/Common/StringInterner.cpp
        src/Lexer/Lexer.cpp
        src/Parser/Parser.cpp
        src/Sema/SymbolTable.cpp
)

# C++20 Module Interfaces directly inside src/
target_sources(foolc
        PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS src
        FILES
        src/Common/Common.ixx
        src/Lexer/Lexer.ixx
        src/AST/AST.ixx
        src/Parser/Parser.ixx
        src/Sema/SymbolTable.ixx
)

if (MSVC)
    target_compile_options(foolc PRIVATE /W4 /permissive-)
else()
    target_compile_options(foolc PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ==============================================================================
# CTest Integration: Testing foolc compiler on Fool source files
# ==============================================================================
include(CTest)
enable_testing()

if (BUILD_TESTING)
    # 1. Default test suite runner
    add_test(
        NAME foolc_default_suite
        COMMAND foolc
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    # 2. Individual tests for each example file (.fo, .fl, .fex)
    file(GLOB_RECURSE FOOL_EXAMPLE_FILES
        LIST_DIRECTORIES false
        "${CMAKE_SOURCE_DIR}/examples/*.fo"
        "${CMAKE_SOURCE_DIR}/examples/*.fl"
        "${CMAKE_SOURCE_DIR}/examples/*.fex"
    )

    foreach(TEST_FILE ${FOOL_EXAMPLE_FILES})
        file(RELATIVE_PATH REL_TEST_FILE "${CMAKE_SOURCE_DIR}" "${TEST_FILE}")
        string(REGEX REPLACE "[/\\\\]" "_" TEST_NAME_CLEAN "${REL_TEST_FILE}")
        string(REGEX REPLACE "\\." "_" TEST_NAME_CLEAN "${TEST_NAME_CLEAN}")

        add_test(
            NAME "foolc_test_${TEST_NAME_CLEAN}"
            COMMAND foolc "${TEST_FILE}"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
    endforeach()

    # 3. Individual tests for standard library files (.fo, .fl, .fex)
    file(GLOB_RECURSE FOOL_STDLIB_FILES
        LIST_DIRECTORIES false
        "${CMAKE_SOURCE_DIR}/stdlib/*.fo"
        "${CMAKE_SOURCE_DIR}/stdlib/*.fl"
        "${CMAKE_SOURCE_DIR}/stdlib/*.fex"
    )

    foreach(STDLIB_FILE ${FOOL_STDLIB_FILES})
        file(RELATIVE_PATH REL_STDLIB_FILE "${CMAKE_SOURCE_DIR}" "${STDLIB_FILE}")
        string(REGEX REPLACE "[/\\\\]" "_" TEST_NAME_CLEAN "${REL_STDLIB_FILE}")
        string(REGEX REPLACE "\\." "_" TEST_NAME_CLEAN "${TEST_NAME_CLEAN}")

        add_test(
            NAME "foolc_test_${TEST_NAME_CLEAN}"
            COMMAND foolc "${STDLIB_FILE}"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
    endforeach()
endif()
