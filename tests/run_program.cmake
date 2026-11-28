# Builds a Fool program with `foolc --build`, runs it and checks its exit status and output.
#
#   cmake -DFOOLC=<foolc> -DINPUT=<file.fl> -DWORK=<dir> [-DCLANG=<clang>] -P run_program.cmake
#
# The expectations are comments in the source:
#   // expect-exit: <n>          the exit status of the program (default 0)
#   // expect-output: <text>     one line of its output; `\n` stands for a newline. Several lines are joined in order.

cmake_minimum_required(VERSION 3.20)

foreach(REQUIRED FOOLC INPUT WORK)
    if(NOT DEFINED ${REQUIRED})
        message(FATAL_ERROR "${REQUIRED} is not set")
    endif()
endforeach()

get_filename_component(NAME "${INPUT}" NAME_WE)
file(MAKE_DIRECTORY "${WORK}")
set(EXE "${WORK}/${NAME}${CMAKE_EXECUTABLE_SUFFIX}")
file(REMOVE "${EXE}")

file(STRINGS "${INPUT}" LINES)
set(WANT_EXIT 0)
set(WANT_OUTPUT "")
foreach(LINE ${LINES})
    if(LINE MATCHES "^// expect-exit: *(-?[0-9]+)")
        set(WANT_EXIT "${CMAKE_MATCH_1}")
    elseif(LINE MATCHES "^// expect-output: ?(.*)$")
        string(APPEND WANT_OUTPUT "${CMAKE_MATCH_1}")
    endif()
endforeach()
string(REPLACE "\\n" "\n" WANT_OUTPUT "${WANT_OUTPUT}")

set(CLANG_ARGS "")
if(DEFINED CLANG AND NOT CLANG STREQUAL "")
    set(CLANG_ARGS --clang "${CLANG}")
endif()

execute_process(COMMAND "${FOOLC}" -q --build ${CLANG_ARGS} "${INPUT}" -o "${EXE}"
                OUTPUT_VARIABLE BUILD_OUT ERROR_VARIABLE BUILD_ERR RESULT_VARIABLE BUILD_RC)
if(NOT BUILD_RC EQUAL 0)
    message(FATAL_ERROR "foolc --build failed (${BUILD_RC}) on ${INPUT}:\n${BUILD_OUT}${BUILD_ERR}")
endif()

execute_process(COMMAND "${EXE}" OUTPUT_VARIABLE RUN_OUT ERROR_VARIABLE RUN_ERR RESULT_VARIABLE RUN_RC)
string(REPLACE "\r\n" "\n" RUN_OUT "${RUN_OUT}")
if(NOT RUN_RC STREQUAL "${WANT_EXIT}")
    message(FATAL_ERROR "${NAME}: exit status ${RUN_RC}, expected ${WANT_EXIT}\n${RUN_OUT}${RUN_ERR}")
endif()
if(NOT RUN_OUT STREQUAL WANT_OUTPUT)
    message(FATAL_ERROR "${NAME}: output differs\n--- expected\n${WANT_OUTPUT}\n--- actual\n${RUN_OUT}")
endif()
message(STATUS "${NAME}: exit ${RUN_RC}, output as expected")
