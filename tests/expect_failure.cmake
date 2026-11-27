# Runs a command that is EXPECTED to be rejected, and passes (exits 0) only when
#   - the command exits with a plain non-zero status (a diagnosed failure: not a crash and not a signal), and
#   - everything it printed (stdout and stderr) matches REGEX.
# The test then reads as a pass everywhere (ctest, CLion, CI) instead of relying on PASS_REGULAR_EXPRESSION to
# override the command's own non-zero exit code.
#
#   cmake -DREGEX=<regex> -P expect_failure.cmake <command> [args...]

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED REGEX)
    message(FATAL_ERROR "expect_failure.cmake: pass -DREGEX=<regex>")
endif()

# The command is whatever follows `-P <this script>` on our own command line.
set(command "")
set(seen_script FALSE)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(i RANGE 0 ${last})
    if(seen_script)
        list(APPEND command "${CMAKE_ARGV${i}}")
    elseif("${CMAKE_ARGV${i}}" STREQUAL "-P")
        set(seen_script TRUE)
        set(skip_next TRUE)
    endif()
endforeach()
list(POP_FRONT command)   # the script path itself
if(command STREQUAL "")
    message(FATAL_ERROR "expect_failure.cmake: no command to run")
endif()

execute_process(COMMAND ${command}
                RESULT_VARIABLE status
                OUTPUT_VARIABLE out
                ERROR_VARIABLE err)
set(output "${out}${err}")

if(NOT status MATCHES "^[0-9]+$")
    message(FATAL_ERROR "the command crashed or was killed (${status}) instead of reporting an error\n${output}")
endif()
if(status EQUAL 0)
    message(FATAL_ERROR "the command succeeded, but a rejection was expected\n${output}")
endif()
if(status GREATER 127)
    message(FATAL_ERROR "the command exited with ${status}, which looks like a crash\n${output}")
endif()
if(NOT output MATCHES "${REGEX}")
    message(FATAL_ERROR "the command failed (exit ${status}) but its output does not match\n  ${REGEX}\n--- output ---\n${output}")
endif()
message(STATUS "rejected as expected (exit ${status}); output matches")
