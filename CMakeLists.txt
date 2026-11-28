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
        src/Sema/TypeCheckerOptional.cpp
        src/Sema/TypeCheckerLambda.cpp
        src/Sema/TypeCheckerSerialize.cpp
        src/Sema/BorrowChecker.cpp
        src/HIR/HIR.cpp
        src/HIR/Lower.cpp
        src/MIR/MIR.cpp
        src/MIR/Build.cpp
        src/Codegen/LLVM/LLVMCodeGen.cpp
        src/Module/Sha256.cpp
        src/Module/Toml.cpp
        src/Module/TomlSelfTest.cpp
        src/Module/Manifest.cpp
        src/Module/BMICodec.cpp
        src/Module/BMICache.cpp
        src/Module/BMISelfTest.cpp
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
        src/HIR/HIR.ixx
        src/MIR/MIR.ixx
        src/Codegen/LLVM/LLVMCodeGen.ixx
        src/Module/BMICache.ixx
        src/Module/Toml.ixx
        src/Module/Manifest.ixx
)

if (MSVC)
    target_compile_options(foolc PRIVATE /W4 /permissive-)
else()
    target_compile_options(foolc PRIVATE -Wall -Wextra -Wpedantic)
endif()

# .flbmi cache entries are keyed on a fingerprint of everything that decides what the parser produces
# (lexer, parser, AST, arena, codec), so a compiler built from different frontend sources never reuses
# an entry written by another one. Editing one of these files re-runs CMake and refreshes the hash.
set(FOOL_FRONTEND_SOURCES
        src/Common/Common.ixx src/Lexer/Lexer.ixx src/Lexer/Lexer.cpp src/AST/AST.ixx
        src/Parser/Parser.ixx src/Parser/Parser.cpp src/Parser/ParserDecl.cpp
        src/Module/BMICache.ixx src/Module/BMICodec.cpp)
set(FOOL_FRONTEND_HASHES "")
foreach(FRONTEND_SOURCE ${FOOL_FRONTEND_SOURCES})
    file(SHA256 "${CMAKE_SOURCE_DIR}/${FRONTEND_SOURCE}" FRONTEND_SOURCE_HASH)
    string(APPEND FOOL_FRONTEND_HASHES "${FRONTEND_SOURCE}:${FRONTEND_SOURCE_HASH};")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${FRONTEND_SOURCE}")
endforeach()
string(SHA256 FOOL_FRONTEND_FINGERPRINT "${FOOL_FRONTEND_HASHES}")
set_source_files_properties(src/Module/BMICache.cpp PROPERTIES
        COMPILE_DEFINITIONS "FOOL_FRONTEND_FINGERPRINT=\"${FOOL_FRONTEND_FINGERPRINT}\"")

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
    # A test labelled `negative` expects foolc to reject the project: tests/expect_failure.cmake runs it, requires the
    # non-zero exit and the message, and exits 0 itself, so the test is a plain pass for ctest, CLion and CI. Any other
    # test passes when foolc succeeds and its output matches.
    function(add_foolc_run_test NAME DIR REGEX LABELS)
        set(extra_flags ${ARGN})
        if("negative" IN_LIST LABELS)
            add_test(NAME "${NAME}"
                     COMMAND ${CMAKE_COMMAND} "-DREGEX=${REGEX}" -P "${CMAKE_SOURCE_DIR}/tests/expect_failure.cmake"
                             $<TARGET_FILE:foolc> --project ${extra_flags} -I "tests/project/${DIR}"
                     WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
            set_tests_properties("${NAME}" PROPERTIES LABELS "${LABELS}")
        else()
            add_test(NAME "${NAME}"
                     COMMAND foolc --project ${extra_flags} -I "tests/project/${DIR}"
                     WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
            set_tests_properties("${NAME}" PROPERTIES LABELS "${LABELS}" PASS_REGULAR_EXPRESSION "${REGEX}")
        endif()
    endfunction()

    function(add_project_test NAME DIR REGEX LABELS)
        add_foolc_run_test("${NAME}" "${DIR}" "${REGEX}" "${LABELS}")
    endfunction()

    add_project_test(foolc_project_templates_ok ok "1 instantiation.s.\\), 0 error" "project;positive")
    add_project_test(foolc_project_cycle cycle "circular module dependency" "project;negative")
    add_project_test(foolc_project_unresolved unresolved "unresolved import" "project;negative")
    add_project_test(foolc_project_bad_template bad_template "has no template parameter" "project;negative")
    # An interface (.fo) without an implementation file (.fl) is valid; calls across the import are still type-checked.
    add_project_test(foolc_project_interface_only interface_only "2 module.s. from 2 file.s.*, 0 error" "project;positive")
    add_project_test(foolc_project_import_type_mismatch import_type_mismatch
                     "cannot convert 'i64' to 'i32'.*argument 'scale' of call to 'geometry::dot'" "project;negative")

    # Visibility: `pub`, the privacy unit (same-stem files), and namespaces across files.
    add_project_test(foolc_project_vis_ok vis_ok "3 module.s. from 5 file.s.*, 0 error" "project;positive")
    add_project_test(foolc_project_vis_private_function vis_private "function 'internal_helper' is private to 'lib.fo'" "project;negative")
    add_project_test(foolc_project_vis_private_other_file vis_private "other.fl:[0-9]+:[0-9]+: error: function 'internal_helper' is private" "project;negative")
    add_project_test(foolc_project_vis_private_field vis_private "field 'closed' of 'Box' is private to 'lib.fo'; declare it `pub` inside the class" "project;negative")
    add_project_test(foolc_project_vis_private_method vis_private "method 'Box::peek' is private to 'lib.fo'" "project;negative")
    add_project_test(foolc_project_vis_private_constant vis_private "constant 'HIDDEN_CONST' is private to 'lib.fo'" "project;negative")
    add_project_test(foolc_project_vis_private_type vis_private "type 'Hidden' is private to 'lib.fo'" "project;negative")
    add_project_test(foolc_project_vis_private_namespace vis_private "function 'closed_ns::g' is private to 'lib.fo'" "project;negative")
    add_project_test(foolc_project_vis_note vis_private "note: 'internal_helper' is declared here without `pub`" "project;negative")

    # Path imports: roots (@src, @lib, @sup, @std, relative), `{a, b} from`, `pub #import`, files loaded on demand.
    add_project_test(foolc_project_import_forms import_forms "checked [0-9]+ module.s. from [0-9]+ file.s. for linux-x86_64 .0 instantiation.s.., 0 error" "project;positive")
    add_project_test(foolc_project_import_lazy lazy/app "2 module.s. from 2 file.s.*, 0 error" "project;positive")
    add_project_test(foolc_project_import_missing_name import_names "'nothing' is not declared in 'math'" "project;negative")
    add_project_test(foolc_project_import_private_name import_names "'hidden' is private to 'math.fl'" "project;negative")
    add_project_test(foolc_project_import_not_selected import_names "main.fl:8:[0-9]+: error: .*'mul'" "project;negative")
    add_project_test(foolc_project_import_reexport_narrowed import_names "reexport.fl:[0-9]+:[0-9]+: error: .*'whisper'" "project;negative")
    add_project_test(foolc_project_import_no_package import_errors "package 'nothere' is not installed" "project;negative")
    add_project_test(foolc_project_import_no_file import_errors "unresolved import 'nowhere:file': no file or folder '[^']*nowhere/file'" "project;negative")
    add_project_test(foolc_project_import_drive import_errors "unresolved import ':z:nothing': no file or folder 'Z:/nothing'" "project;negative")

    # 2d. fool.toml / overwrite.toml: package lookup in the project's `packages` folder and in shared collections.
    function(add_package_test NAME DIR REGEX LABELS)
        add_foolc_run_test("${NAME}" "${DIR}" "${REGEX}" "${LABELS}" --show-packages)
    endfunction()

    add_test(NAME foolc_toml_selftest COMMAND foolc --toml-selftest)
    set_tests_properties(foolc_toml_selftest PROPERTIES LABELS "project;packages"
                         PASS_REGULAR_EXPRESSION "toml selftest: ok .*manifest selftest: ok")
    # Only what is imported is loaded: the unused package next to `shapes` has a syntax error and is never read.
    add_package_test(foolc_pkg_local pkg_local "project geometry-app 0.3.0.*shapes 1 -> tests/project/pkg_local/packages/shapes.*checked 2 module.s. from 2 file.s.*, 0 error" "project;packages;positive")
    add_package_test(foolc_pkg_global pkg_global "collections:.*pkg_global/collection.*mathx 2 -> tests/project/pkg_global/collection/mathx.*localtool [*] -> tests/project/pkg_global/packages/localtool.*checked 3 module.s. from 3 file.s.*, 0 error" "project;packages;positive")
    add_package_test(foolc_pkg_override pkg_override "tools -> tests/project/pkg_override/checkout/tools.*checked 2 module.s. from 2 file.s.*, 0 error" "project;packages;positive")
    add_package_test(foolc_pkg_chain pkg_chain "outer 1 -> tests/project/pkg_chain/packages/outer.*checked 3 module.s. from 3 file.s.*, 0 error" "project;packages;positive")
    add_package_test(foolc_pkg_renamed_folders pkg_folders "project renamed-folders .*src: source, lib: vendor.*checked 3 module.s. from 3 file.s.*, 0 error" "project;packages;positive")
    add_package_test(foolc_pkg_version pkg_version "main.fl:3:1: error: unresolved import '@pack:shapes:area': package 'shapes' 1.2.0 in '[^']*packages/shapes' does not satisfy the required version '2'" "project;packages;negative")
    add_package_test(foolc_pkg_git_source pkg_missing "package 'remote' is not installed .expected in '[^']*packages/remote'; also looked in '[^']*shared-one/remote', '[^']*shared-two/remote'.; its git source 'https://example.com/remote.git' needs the package manager" "project;packages;negative")
    add_package_test(foolc_pkg_bad_path pkg_missing "dependency 'nopath' points to '[^']*does/not/exist', which is not a folder" "project;packages;negative")
    add_package_test(foolc_pkg_undeclared pkg_missing "package 'undeclared' is not installed" "project;packages;negative")
    add_package_test(foolc_pkg_manifest_types pkg_badtoml "fool.toml:3:11: error: 'package.version' must be a string, not an integer" "project;packages;negative")
    add_package_test(foolc_pkg_manifest_folder pkg_badtoml "fool.toml:4:7: error: 'src' must be a folder inside the project, not '..'" "project;packages;negative")
    # stdout and stderr are two streams: each message gets its own test rather than relying on how they interleave.
    add_package_test(foolc_pkg_manifest_folder_default pkg_badtoml "src: src, lib: lib" "project;packages;negative")
    add_package_test(foolc_pkg_manifest_dependency pkg_badtoml "fool.toml:9:9: error: dependency 'wrong' must be a version string or a table, not an integer" "project;packages;negative")
    add_package_test(foolc_pkg_manifest_dependency_path pkg_badtoml "fool.toml:10:34: error: 'path' must be a string, not an integer" "project;packages;negative")
    add_package_test(foolc_pkg_overwrite_own_file pkg_badtoml "overwrite.toml:2:15: error: 'packages.collections' must be an array of paths, not a string" "project;packages;negative")
    add_package_test(foolc_pkg_unknown_key pkg_badtoml "overwrite.toml:4:10: warning: unknown key 'colour' in .packages. .ignored." "project;packages;negative")
    add_package_test(foolc_pkg_unknown_table pkg_badtoml "fool.toml:12:1: warning: unknown table 'mystery' .ignored." "project;packages;negative")
    # Diagnostics print the file path the way the OS writes it (`tests\project\x\fool.toml` on Windows), so these regexes
    # start at the file name; messages that quote a path (`generic_string`) always use `/`.
    add_package_test(foolc_pkg_syntax_error pkg_syntax "fool.toml:3:11: error: unterminated string" "project;packages;negative")
    add_package_test(foolc_pkg_syntax_error_overwrite pkg_syntax_overwrite "overwrite.toml:2:21: error: expected a value" "project;packages;negative")

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
    # -Werror turns the shadowing warnings into a failing exit; expect_failure.cmake makes that a plain pass.
    add_test(NAME foolc_warn_werror_fails
             COMMAND ${CMAKE_COMMAND} "-DREGEX=declaration of 'total' shadows a declaration" -P "${CMAKE_SOURCE_DIR}/tests/expect_failure.cmake"
                     $<TARGET_FILE:foolc> -q --warn-shadow -Werror "${CMAKE_SOURCE_DIR}/tests/positive/shadowing.fl"
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(foolc_warn_werror_fails PROPERTIES LABELS "warn")

    # 9. .flbmi cache. The self-test checks SHA-256, the key, the codec (round trip, truncation and
    #    bit flips) and the on-disk behaviour. The round-trip runs repeat the groups above on the tree
    #    decoded from the encoded file, which is exactly what a cache hit hands to the type checker.
    #    cache_cycle.cmake drives a cold/warm/edited/damaged sequence through the real binary.
    add_test(NAME foolc_bmi_selftest
             COMMAND foolc --bmi-selftest ${FOOL_SOURCE_FILES} ${FOOL_POSITIVE_FILES}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME foolc_bmi_roundtrip_sources
             COMMAND foolc -q --bmi-roundtrip ${FOOL_SOURCE_FILES}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME foolc_bmi_roundtrip_positive
             COMMAND foolc -q --bmi-roundtrip ${FOOL_POSITIVE_FILES}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME foolc_bmi_roundtrip_negative
             COMMAND foolc -q --bmi-roundtrip ${FOOL_NEGATIVE_FILES}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME foolc_bmi_roundtrip_borrow
             COMMAND foolc -q --bmi-roundtrip --check-borrow ${FOOL_BORROW_FILES}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME foolc_bmi_roundtrip_warn
             COMMAND foolc -q --bmi-roundtrip --warn-shadow ${FOOL_WARN_FILES}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME foolc_bmi_roundtrip_project
             COMMAND foolc --project --bmi-roundtrip --check-borrow --warn-shadow -I stdlib -I examples
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    string(REPLACE ";" "|" FOOL_NEGATIVE_FILE_ARG "${FOOL_NEGATIVE_FILES}")
    add_test(NAME foolc_bmi_cache_cycle
             COMMAND ${CMAKE_COMMAND}
                     -DFOOLC=$<TARGET_FILE:foolc>
                     -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                     -DWORK_DIR=${CMAKE_BINARY_DIR}/bmi-cache-test
                     "-DNEGATIVE_FILES_ARG=${FOOL_NEGATIVE_FILE_ARG}"
                     -P ${CMAKE_SOURCE_DIR}/tests/cache/cache_cycle.cmake)
    # 10. Bound calls (`--dump-calls`): named arguments in canonical slots with defaults filled, compared
    #     with golden files; the project run checks the same shape on examples/03_middle_defaults.
    file(GLOB FOOL_CALL_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/calls/*.fl")
    foreach(CALL_FILE ${FOOL_CALL_FILES})
        get_filename_component(CALL_NAME "${CALL_FILE}" NAME_WE)
        add_test(NAME "foolc_calls_${CALL_NAME}"
                 COMMAND ${CMAKE_COMMAND} -DFOOLC=$<TARGET_FILE:foolc> -DINPUT=${CALL_FILE}
                         -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/calls/${CALL_NAME}.expected
                         -P ${CMAKE_SOURCE_DIR}/tests/calls/run_golden.cmake)
        set_tests_properties("foolc_calls_${CALL_NAME}" PROPERTIES LABELS "calls")
        # Same output (columns included) on the tree decoded from the .flbmi encoding.
        add_test(NAME "foolc_calls_${CALL_NAME}_roundtrip"
                 COMMAND ${CMAKE_COMMAND} -DFOOLC=$<TARGET_FILE:foolc> -DINPUT=${CALL_FILE} -DEXTRA=--bmi-roundtrip
                         -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/calls/${CALL_NAME}.expected
                         -P ${CMAKE_SOURCE_DIR}/tests/calls/run_golden.cmake)
        set_tests_properties("foolc_calls_${CALL_NAME}_roundtrip" PROPERTIES LABELS "calls;bmi")
    endforeach()
    # 11. Expression types (`--dump-types`), same golden-file mechanism.
    file(GLOB FOOL_TYPE_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/types/*.fl")
    foreach(TYPE_FILE ${FOOL_TYPE_FILES})
        get_filename_component(TYPE_NAME "${TYPE_FILE}" NAME_WE)
        foreach(VARIANT plain roundtrip)
            set(EXTRA_FLAG "")
            set(TEST_SUFFIX "")
            set(TEST_LABELS "types")
            if(VARIANT STREQUAL "roundtrip")
                set(EXTRA_FLAG "--bmi-roundtrip")
                set(TEST_SUFFIX "_roundtrip")
                set(TEST_LABELS "types;bmi")
            endif()
            add_test(NAME "foolc_types_${TYPE_NAME}${TEST_SUFFIX}"
                     COMMAND ${CMAKE_COMMAND} -DFOOLC=$<TARGET_FILE:foolc> -DINPUT=${TYPE_FILE} -DFLAG=--dump-types
                             -DEXTRA=${EXTRA_FLAG} -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/types/${TYPE_NAME}.expected
                             -P ${CMAKE_SOURCE_DIR}/tests/calls/run_golden.cmake)
            set_tests_properties("foolc_types_${TYPE_NAME}${TEST_SUFFIX}" PROPERTIES LABELS "${TEST_LABELS}")
        endforeach()
    endforeach()
    # 12. Reflection metadata (`--dump-reflection`), same golden-file mechanism.
    file(GLOB FOOL_REFLECTION_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/reflection/*.fl")
    foreach(REFLECTION_FILE ${FOOL_REFLECTION_FILES})
        get_filename_component(REFLECTION_NAME "${REFLECTION_FILE}" NAME_WE)
        foreach(VARIANT plain roundtrip)
            set(EXTRA_FLAG "")
            set(TEST_SUFFIX "")
            set(TEST_LABELS "reflection")
            if(VARIANT STREQUAL "roundtrip")
                set(EXTRA_FLAG "--bmi-roundtrip")
                set(TEST_SUFFIX "_roundtrip")
                set(TEST_LABELS "reflection;bmi")
            endif()
            add_test(NAME "foolc_reflection_${REFLECTION_NAME}${TEST_SUFFIX}"
                     COMMAND ${CMAKE_COMMAND} -DFOOLC=$<TARGET_FILE:foolc> -DINPUT=${REFLECTION_FILE} -DFLAG=--dump-reflection
                             -DEXTRA=${EXTRA_FLAG} -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/reflection/${REFLECTION_NAME}.expected
                             -P ${CMAKE_SOURCE_DIR}/tests/calls/run_golden.cmake)
            set_tests_properties("foolc_reflection_${REFLECTION_NAME}${TEST_SUFFIX}" PROPERTIES LABELS "${TEST_LABELS}")
        endforeach()
    endforeach()
    # 13. HIR lowering (`--dump-hir`), same golden-file mechanism.
    file(GLOB FOOL_HIR_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/hir/*.fl")
    foreach(HIR_FILE ${FOOL_HIR_FILES})
        get_filename_component(HIR_NAME "${HIR_FILE}" NAME_WE)
        foreach(VARIANT plain roundtrip)
            set(EXTRA_FLAG "")
            set(TEST_SUFFIX "")
            set(TEST_LABELS "hir")
            if(VARIANT STREQUAL "roundtrip")
                set(EXTRA_FLAG "--bmi-roundtrip")
                set(TEST_SUFFIX "_roundtrip")
                set(TEST_LABELS "hir;bmi")
            endif()
            add_test(NAME "foolc_hir_${HIR_NAME}${TEST_SUFFIX}"
                     COMMAND ${CMAKE_COMMAND} -DFOOLC=$<TARGET_FILE:foolc> -DINPUT=${HIR_FILE} -DFLAG=--dump-hir
                             -DEXTRA=${EXTRA_FLAG} -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/hir/${HIR_NAME}.expected
                             -P ${CMAKE_SOURCE_DIR}/tests/calls/run_golden.cmake)
            set_tests_properties("foolc_hir_${HIR_NAME}${TEST_SUFFIX}" PROPERTIES LABELS "${TEST_LABELS}")
        endforeach()
    endforeach()
    # 14. MIR (`--dump-mir`): golden files; the verifier runs on every dump and reports failures as errors.
    file(GLOB FOOL_MIR_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/mir/*.fl")
    foreach(MIR_FILE ${FOOL_MIR_FILES})
        get_filename_component(MIR_NAME "${MIR_FILE}" NAME_WE)
        foreach(VARIANT plain roundtrip)
            set(EXTRA_FLAG "")
            set(TEST_SUFFIX "")
            set(TEST_LABELS "mir")
            if(VARIANT STREQUAL "roundtrip")
                set(EXTRA_FLAG "--bmi-roundtrip")
                set(TEST_SUFFIX "_roundtrip")
                set(TEST_LABELS "mir;bmi")
            endif()
            add_test(NAME "foolc_mir_${MIR_NAME}${TEST_SUFFIX}"
                     COMMAND ${CMAKE_COMMAND} -DFOOLC=$<TARGET_FILE:foolc> -DINPUT=${MIR_FILE} -DFLAG=--dump-mir
                             -DEXTRA=${EXTRA_FLAG} -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/mir/${MIR_NAME}.expected
                             -P ${CMAKE_SOURCE_DIR}/tests/calls/run_golden.cmake)
            set_tests_properties("foolc_mir_${MIR_NAME}${TEST_SUFFIX}" PROPERTIES LABELS "${TEST_LABELS}")
        endforeach()
    endforeach()
    add_test(NAME foolc_mir_selftest COMMAND foolc --mir-selftest)
    set_tests_properties(foolc_mir_selftest PROPERTIES LABELS "mir" PASS_REGULAR_EXPRESSION "mir selftest: ok")
    # Every function of stdlib/ and examples/ that HIR lowers must also build into a verified MIR graph.
    add_test(NAME foolc_mir_project
             COMMAND foolc -q --project --dump-mir -I stdlib -I examples
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(foolc_mir_project PROPERTIES LABELS "mir;project")
    add_test(NAME foolc_calls_project
             COMMAND foolc --project --dump-calls -I stdlib -I examples
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(foolc_calls_project PROPERTIES LABELS "calls;project"
        PASS_REGULAR_EXPRESSION
        "call render_box \\([0-9]+:[0-9]+\\): x=arg0 y=arg1 scale=default width=arg2 height=arg3 depth=default")

    # 15. LLVM backend (`--emit-llvm`): golden IR files, with the same plain / BMI-roundtrip variants as above.
    file(GLOB FOOL_LLVM_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/llvm/*.fl")
    foreach(LLVM_FILE ${FOOL_LLVM_FILES})
        get_filename_component(LLVM_NAME "${LLVM_FILE}" NAME_WE)
        foreach(VARIANT plain roundtrip)
            set(EXTRA_FLAG "")
            set(TEST_SUFFIX "")
            set(TEST_LABELS "llvm")
            if(VARIANT STREQUAL "roundtrip")
                set(EXTRA_FLAG "--bmi-roundtrip")
                set(TEST_SUFFIX "_roundtrip")
                set(TEST_LABELS "llvm;bmi")
            endif()
            add_test(NAME "foolc_llvm_${LLVM_NAME}${TEST_SUFFIX}"
                     COMMAND ${CMAKE_COMMAND} -DFOOLC=$<TARGET_FILE:foolc> -DINPUT=${LLVM_FILE} -DFLAG=--emit-llvm
                             -DEXTRA=${EXTRA_FLAG} -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/llvm/${LLVM_NAME}.expected
                             -P ${CMAKE_SOURCE_DIR}/tests/calls/run_golden.cmake)
            set_tests_properties("foolc_llvm_${LLVM_NAME}${TEST_SUFFIX}" PROPERTIES LABELS "${TEST_LABELS}")
        endforeach()
    endforeach()
    # `--build` refuses a program with functions the backend cannot emit, and a program without `fn main`.
    add_test(NAME foolc_llvm_build_unsupported
             COMMAND ${CMAKE_COMMAND} "-DREGEX=3 function\\(s\\) cannot be compiled to LLVM yet"
                     -P "${CMAKE_SOURCE_DIR}/tests/expect_failure.cmake"
                     $<TARGET_FILE:foolc> -q --build ${CMAKE_SOURCE_DIR}/tests/llvm/unsupported.fl
                     -o ${CMAKE_BINARY_DIR}/tests_llvm/unsupported${CMAKE_EXECUTABLE_SUFFIX})
    add_test(NAME foolc_llvm_build_no_main
             COMMAND ${CMAKE_COMMAND} "-DREGEX=the program has no 'fn main\\(\\)'"
                     -P "${CMAKE_SOURCE_DIR}/tests/expect_failure.cmake"
                     $<TARGET_FILE:foolc> -q --build ${CMAKE_SOURCE_DIR}/tests/llvm/scalars.fl
                     -o ${CMAKE_BINARY_DIR}/tests_llvm/scalars${CMAKE_EXECUTABLE_SUFFIX})
    set_tests_properties(foolc_llvm_build_unsupported foolc_llvm_build_no_main PROPERTIES LABELS "llvm")
    # Programs that are built with clang, run, and checked for their exit status and output (tests/run_program.cmake).
    # Needs a clang at test time; without one these tests are simply not registered.
    find_program(FOOL_CLANG NAMES clang clang-18 clang-19 clang-20)
    if(FOOL_CLANG)
        file(GLOB FOOL_RUN_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/run/*.fl")
        foreach(RUN_FILE ${FOOL_RUN_FILES})
            get_filename_component(RUN_NAME "${RUN_FILE}" NAME_WE)
            add_test(NAME "foolc_run_${RUN_NAME}"
                     COMMAND ${CMAKE_COMMAND} -DFOOLC=$<TARGET_FILE:foolc> -DINPUT=${RUN_FILE} -DCLANG=${FOOL_CLANG}
                             -DWORK=${CMAKE_BINARY_DIR}/tests_run -P ${CMAKE_SOURCE_DIR}/tests/run_program.cmake)
            set_tests_properties("foolc_run_${RUN_NAME}" PROPERTIES LABELS "llvm;run")
        endforeach()
    else()
        message(STATUS "clang not found: the tests that build and run programs (tests/run) are not registered")
    endif()

    set_tests_properties(foolc_bmi_selftest foolc_bmi_roundtrip_sources foolc_bmi_roundtrip_positive
                         foolc_bmi_roundtrip_negative foolc_bmi_roundtrip_borrow foolc_bmi_roundtrip_warn
                         foolc_bmi_roundtrip_project foolc_bmi_cache_cycle PROPERTIES LABELS "bmi")
endif()
