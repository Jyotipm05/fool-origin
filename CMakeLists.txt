cmake_minimum_required(VERSION 3.28)

project(foolc
        VERSION 0.1.0
        LANGUAGES CXX
        DESCRIPTION "The Fool Systems Programming Language Compiler Frontend"
)

# C++23 named modules need a generator that understands module dependency scanning (Ninja or
# Visual Studio) and a recent compiler (clang >= 17 with clang-scan-deps, GCC >= 14, MSVC 19.34+).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_GENERATOR MATCHES "Ninja|Visual Studio")
    message(WARNING "foolc uses C++ named modules; configure with -G Ninja (see CMakePresets.json).")
endif()

# Put the compiler where the README says it is.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Compiler Binary
add_executable(foolc
        src/main.cpp
        src/Common/StringInterner.cpp
        src/Lexer/Lexer.cpp
        src/Parser/Parser.cpp
        src/Parser/ParserDecl.cpp
        src/Sema/ConstEval.cpp
        src/Sema/SymbolTable.cpp
        src/Sema/ModuleGraph.cpp
        src/Sema/TypeChecker.cpp
        src/Sema/TypeCheckerExpr.cpp
        src/Sema/TypeCheckerCasts.cpp
        src/Sema/BorrowChecker.cpp
)

# C++23 Module Interfaces directly inside src/
target_sources(foolc
        PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS src
        FILES
        src/Common/Common.ixx
        src/Common/Diagnostics.ixx
        src/Lexer/Lexer.ixx
        src/AST/AST.ixx
        src/Parser/Parser.ixx
        src/Sema/SymbolTable.ixx
        src/Sema/BorrowChecker.ixx
)

if (MSVC)
    target_compile_options(foolc PRIVATE /W4 /permissive-)
else()
    target_compile_options(foolc PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ==============================================================================
# CTest Integration
# ==============================================================================
include(CTest)

if (BUILD_TESTING)
    # 1. Whole-workspace check: resolves every import, orders the module graph and runs the
    #    type checker over stdlib/ and examples/ (including import-template instantiations).
    add_test(NAME foolc_project_check
             COMMAND foolc --project -I stdlib -I examples
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(foolc_project_check PROPERTIES LABELS "smoke;project")

    # 2. The same workspace for other hosted targets, to exercise @target / @if / platform file selection.
    #    (wasm/wasi are not listed: the stdlib has no std::io / std::net backend for them yet.)
    foreach(TARGET_TRIPLE linux-aarch64 darwin-aarch64 windows-x86_64)
        add_test(NAME "foolc_project_check_${TARGET_TRIPLE}"
                 COMMAND foolc --project --target ${TARGET_TRIPLE} -I stdlib -I examples
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties("foolc_project_check_${TARGET_TRIPLE}" PROPERTIES LABELS "project;targets")
    endforeach()

    # 2b. Project-mode fixtures: a valid import-template instantiation, and graph errors that must
    #     be reported (cycle, unresolved import, unknown template parameter).
    function(add_project_test NAME DIR REGEX LABELS)
        add_test(NAME "${NAME}"
                 COMMAND foolc --project -I "tests/project/${DIR}"
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties("${NAME}" PROPERTIES LABELS "${LABELS}" PASS_REGULAR_EXPRESSION "${REGEX}")
    endfunction()

    add_project_test(foolc_project_templates_ok ok "1 instantiation.s.\\), 0 error" "project;positive")
    add_project_test(foolc_project_cycle cycle "circular module dependency" "project;negative")
    add_project_test(foolc_project_unresolved unresolved "unresolved import" "project;negative")
    add_project_test(foolc_project_bad_template bad_template "has no template parameter" "project;negative")

    # 3. Per-file parse + local checks for every example and stdlib file.
    file(GLOB_RECURSE FOOL_SOURCE_FILES
        LIST_DIRECTORIES false
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/examples/*.fo" "${CMAKE_SOURCE_DIR}/examples/*.fl" "${CMAKE_SOURCE_DIR}/examples/*.fex"
        "${CMAKE_SOURCE_DIR}/stdlib/*.fo"   "${CMAKE_SOURCE_DIR}/stdlib/*.fl"   "${CMAKE_SOURCE_DIR}/stdlib/*.fex"
    )
    foreach(SOURCE_FILE ${FOOL_SOURCE_FILES})
        file(RELATIVE_PATH REL_FILE "${CMAKE_SOURCE_DIR}" "${SOURCE_FILE}")
        string(REGEX REPLACE "[/\\\\.]" "_" TEST_NAME_CLEAN "${REL_FILE}")
        if (REL_FILE MATCHES "^stdlib/")
            set(TEST_LABEL "stdlib")
        else()
            set(TEST_LABEL "examples")
        endif()
        add_test(NAME "foolc_file_${TEST_NAME_CLEAN}"
                 COMMAND foolc -q "${SOURCE_FILE}"
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties("foolc_file_${TEST_NAME_CLEAN}" PROPERTIES LABELS "${TEST_LABEL}")
    endforeach()

    # 4. Negative tests: each file under tests/negative is annotated with `// expect-error:`
    #    comments; foolc succeeds only when exactly those errors are reported.
    file(GLOB_RECURSE FOOL_NEGATIVE_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tests/negative/*.fl" "${CMAKE_SOURCE_DIR}/tests/negative/*.fo")
    foreach(NEGATIVE_FILE ${FOOL_NEGATIVE_FILES})
        get_filename_component(NEGATIVE_NAME "${NEGATIVE_FILE}" NAME_WE)
        add_test(NAME "foolc_negative_${NEGATIVE_NAME}"
                 COMMAND foolc -q "${NEGATIVE_FILE}"
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties("foolc_negative_${NEGATIVE_NAME}" PROPERTIES LABELS "negative")
    endforeach()

    # 5. Positive regression tests (must produce no errors).
    file(GLOB_RECURSE FOOL_POSITIVE_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tests/positive/*.fl" "${CMAKE_SOURCE_DIR}/tests/positive/*.fo")
    foreach(POSITIVE_FILE ${FOOL_POSITIVE_FILES})
        get_filename_component(POSITIVE_NAME "${POSITIVE_FILE}" NAME_WE)
        add_test(NAME "foolc_positive_${POSITIVE_NAME}"
                 COMMAND foolc -q "${POSITIVE_FILE}"
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties("foolc_positive_${POSITIVE_NAME}" PROPERTIES LABELS "positive")
    endforeach()
    # 6. Borrow checker (opt-in): fixtures run with --check-borrow.
    file(GLOB_RECURSE FOOL_BORROW_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tests/borrow/*.fl")
    foreach(BORROW_FILE ${FOOL_BORROW_FILES})
        get_filename_component(BORROW_NAME "${BORROW_FILE}" NAME_WE)
        add_test(NAME "foolc_borrow_${BORROW_NAME}"
                 COMMAND foolc -q --check-borrow "${BORROW_FILE}"
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties("foolc_borrow_${BORROW_NAME}" PROPERTIES LABELS "borrow")
    endforeach()

    # 7. The whole workspace must also be clean under the borrow checker.
    add_test(NAME foolc_project_check_borrow
             COMMAND foolc --project --check-borrow -I stdlib -I examples
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(foolc_project_check_borrow PROPERTIES LABELS "project;borrow")
    # 8. Warnings: fixtures run with --warn-shadow (expect-warning markers); a clean file must also pass -Werror.
    file(GLOB_RECURSE FOOL_WARN_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tests/warn/*.fl")
    foreach(WARN_FILE ${FOOL_WARN_FILES})
        get_filename_component(WARN_NAME "${WARN_FILE}" NAME_WE)
        add_test(NAME "foolc_warn_${WARN_NAME}"
                 COMMAND foolc -q --warn-shadow "${WARN_FILE}"
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties("foolc_warn_${WARN_NAME}" PROPERTIES LABELS "warn")
    endforeach()
    add_test(NAME foolc_warn_clean_werror
             COMMAND foolc -q --warn-shadow -Werror "${CMAKE_SOURCE_DIR}/tests/warn/no_shadow.fl"
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(foolc_warn_clean_werror PROPERTIES LABELS "warn")
    add_test(NAME foolc_warn_werror_fails
             COMMAND foolc -q --warn-shadow -Werror "${CMAKE_SOURCE_DIR}/tests/positive/shadowing.fl"
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(foolc_warn_werror_fails PROPERTIES LABELS "warn" WILL_FAIL TRUE)
endif()
