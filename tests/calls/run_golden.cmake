# Runs `foolc -q <FLAG> <INPUT>` (FLAG defaults to --dump-calls) and compares its output with the golden file EXPECTED.
#
#   cmake -DFOOLC=<foolc> -DINPUT=<file.fl> -DEXPECTED=<file.expected> [-DFLAG=--dump-calls|--dump-types] [-DEXTRA=<flag>] -P run_golden.cmake

cmake_minimum_required(VERSION 3.20)

foreach(REQUIRED FOOLC INPUT EXPECTED)
    if(NOT DEFINED ${REQUIRED})
        message(FATAL_ERROR "${REQUIRED} is not set")
    endif()
endforeach()

if(NOT DEFINED FLAG)
    set(FLAG --dump-calls)
endif()

execute_process(COMMAND "${FOOLC}" -q ${FLAG} ${EXTRA} "${INPUT}"
                OUTPUT_VARIABLE ACTUAL ERROR_VARIABLE ERRORS RESULT_VARIABLE RC)
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "foolc failed (${RC}) on ${INPUT}:\n${ACTUAL}${ERRORS}")
endif()

file(READ "${EXPECTED}" WANTED)
string(REPLACE "\r\n" "\n" ACTUAL "${ACTUAL}")
string(REPLACE "\r\n" "\n" WANTED "${WANTED}")
if(NOT ACTUAL STREQUAL WANTED)
    message(FATAL_ERROR "output differs from ${EXPECTED}\n--- expected\n${WANTED}\n--- actual\n${ACTUAL}")
endif()
