# End-to-end behaviour of the .flbmi cache, driven through the real foolc binary.
#
#   cmake -DFOOLC=<foolc> -DSOURCE_DIR=<repo> -DWORK_DIR=<scratch> [-DNEGATIVE_FILES_ARG=<a|b|...>] -P cache_cycle.cmake
#
# Cold run -> warm run -> edited file -> other target -> damaged entries -> warm run over the negative
# tests. Every run must produce the same module summary as an uncached run.

cmake_minimum_required(VERSION 3.20)

foreach(REQUIRED FOOLC SOURCE_DIR WORK_DIR)
    if(NOT DEFINED ${REQUIRED})
        message(FATAL_ERROR "${REQUIRED} is not set")
    endif()
endforeach()

set(CACHE_DIR "${WORK_DIR}/cache")
set(WORKSPACE "${WORK_DIR}/workspace")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORKSPACE}")
file(COPY "${SOURCE_DIR}/stdlib" "${SOURCE_DIR}/examples" DESTINATION "${WORKSPACE}")

function(run_foolc OUT_OUTPUT OUT_RC)
    execute_process(COMMAND "${FOOLC}" ${ARGN}
                    WORKING_DIRECTORY "${WORK_DIR}"
                    OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERRORS RESULT_VARIABLE RC)
    set(${OUT_OUTPUT} "${OUTPUT}${ERRORS}" PARENT_SCOPE)
    set(${OUT_RC} "${RC}" PARENT_SCOPE)
endfunction()

function(expect_match TEXT REGEX WHAT)
    if(NOT "${TEXT}" MATCHES "${REGEX}")
        message(FATAL_ERROR "${WHAT}: expected /${REGEX}/ in:\n${TEXT}")
    endif()
endfunction()

function(module_summary TEXT OUT)
    string(REGEX MATCH "foolc: checked [^\n]*" LINE "${TEXT}")
    set(${OUT} "${LINE}" PARENT_SCOPE)
endfunction()

set(PROJECT_ARGS --project -I stdlib -I examples)
set(CACHE_ARGS --cache-dir "${CACHE_DIR}")
# foolc is run from WORK_DIR, so the copied roots must sit directly below it.
file(RENAME "${WORKSPACE}/stdlib" "${WORK_DIR}/stdlib")
file(RENAME "${WORKSPACE}/examples" "${WORK_DIR}/examples")

# Reference run without the cache.
run_foolc(REFERENCE RC ${PROJECT_ARGS})
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "uncached run failed:\n${REFERENCE}")
endif()
module_summary("${REFERENCE}" REFERENCE_SUMMARY)

# 1. Cold: everything is a miss and gets stored.
run_foolc(COLD RC ${PROJECT_ARGS} ${CACHE_ARGS})
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "cold run failed:\n${COLD}")
endif()
# (CMake regular expressions have no back-references, so the two counts are compared explicitly.)
if(NOT "${COLD}" MATCHES "0 hit\\(s\\), ([0-9]+) miss\\(es\\), ([0-9]+) stored")
    message(FATAL_ERROR "cold run: unexpected cache statistics in:\n${COLD}")
endif()
set(FILE_COUNT "${CMAKE_MATCH_1}")
if(NOT FILE_COUNT EQUAL CMAKE_MATCH_2 OR FILE_COUNT EQUAL 0)
    message(FATAL_ERROR "cold run: ${FILE_COUNT} misses but ${CMAKE_MATCH_2} stored")
endif()
module_summary("${COLD}" COLD_SUMMARY)
if(NOT COLD_SUMMARY STREQUAL REFERENCE_SUMMARY)
    message(FATAL_ERROR "cold run differs from the uncached run:\n${COLD_SUMMARY}\n${REFERENCE_SUMMARY}")
endif()
file(GLOB ENTRIES "${CACHE_DIR}/*.flbmi")
list(LENGTH ENTRIES ENTRY_COUNT)
if(NOT ENTRY_COUNT EQUAL FILE_COUNT)
    message(FATAL_ERROR "expected ${FILE_COUNT} cache entries, found ${ENTRY_COUNT}")
endif()
file(GLOB LEFTOVERS "${CACHE_DIR}/*.tmp*")
if(LEFTOVERS)
    message(FATAL_ERROR "temporary files left behind: ${LEFTOVERS}")
endif()

# 2. Warm: everything is a hit, nothing is written, results are identical.
run_foolc(WARM RC ${PROJECT_ARGS} ${CACHE_ARGS})
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "warm run failed:\n${WARM}")
endif()
expect_match("${WARM}" "${FILE_COUNT} hit\\(s\\), 0 miss\\(es\\), 0 stored" "warm run")
module_summary("${WARM}" WARM_SUMMARY)
if(NOT WARM_SUMMARY STREQUAL REFERENCE_SUMMARY)
    message(FATAL_ERROR "warm run differs from the uncached run:\n${WARM_SUMMARY}\n${REFERENCE_SUMMARY}")
endif()

# 3. Editing one file invalidates exactly that file.
file(GLOB_RECURSE EDIT_CANDIDATES "${WORK_DIR}/examples/01_basics.fl")
list(GET EDIT_CANDIDATES 0 EDITED)
file(APPEND "${EDITED}" "\n// edited by the cache test\n")
run_foolc(EDITED_RUN RC ${PROJECT_ARGS} ${CACHE_ARGS})
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "run after edit failed:\n${EDITED_RUN}")
endif()
math(EXPR OTHERS "${FILE_COUNT} - 1")
expect_match("${EDITED_RUN}" "${OTHERS} hit\\(s\\), 1 miss\\(es\\), 1 stored" "run after edit")

# 4. Another target is keyed separately (the key contains the target triple).
run_foolc(OTHER_TARGET RC ${PROJECT_ARGS} --target linux-aarch64 ${CACHE_ARGS})
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "other-target run failed:\n${OTHER_TARGET}")
endif()
expect_match("${OTHER_TARGET}" "0 hit\\(s\\), [0-9]+ miss" "other target")

# 5. Damaged entries are rejected, rebuilt, and never change the result.
file(GLOB ENTRIES "${CACHE_DIR}/*.flbmi")
foreach(ENTRY ${ENTRIES})
    file(WRITE "${ENTRY}" "damaged entry")
endforeach()
run_foolc(DAMAGED RC ${PROJECT_ARGS} ${CACHE_ARGS})
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "run over damaged entries failed:\n${DAMAGED}")
endif()
expect_match("${DAMAGED}" "0 hit\\(s\\), [0-9]+ miss\\(es\\), [0-9]+ stored, [0-9]+ rejected" "damaged entries")
run_foolc(REPAIRED RC ${PROJECT_ARGS} ${CACHE_ARGS})
expect_match("${REPAIRED}" "${FILE_COUNT} hit\\(s\\), 0 miss\\(es\\), 0 stored" "repaired cache")

# 6. A read-only / unusable cache location must not break the build.
file(WRITE "${WORK_DIR}/not-a-directory" "x")
run_foolc(UNUSABLE RC ${PROJECT_ARGS} --cache-dir "${WORK_DIR}/not-a-directory/cache")
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "run with an unusable cache directory failed:\n${UNUSABLE}")
endif()
expect_match("${UNUSABLE}" "not stored" "unusable cache directory")

# 7. Files with diagnostics: a warm run must report exactly what the cold run did. The negative tests
#    carry // expect-error: annotations, so a cache hit with wrong line numbers would fail them.
if(DEFINED NEGATIVE_FILES_ARG AND NOT "${NEGATIVE_FILES_ARG}" STREQUAL "")
    string(REPLACE "|" ";" NEGATIVE_FILES "${NEGATIVE_FILES_ARG}")
    foreach(PASS cold warm)
        run_foolc(NEGATIVE_RUN RC -q ${CACHE_ARGS} ${NEGATIVE_FILES})
        if(NOT RC EQUAL 0)
            message(FATAL_ERROR "negative tests failed on the ${PASS} cache run:\n${NEGATIVE_RUN}")
        endif()
    endforeach()
endif()

message(STATUS "cache cycle ok (${FILE_COUNT} files)")
