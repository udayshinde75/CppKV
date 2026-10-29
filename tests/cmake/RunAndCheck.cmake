# Runs a command and checks its exit code (and optionally stderr).
#
#   cmake -DCOMMAND=<exe> -DARGS=<arg;arg> -DEXPECTED_EXIT=<n>
#         [-DEXPECTED_STDERR=<regex>] -P RunAndCheck.cmake
#
# A crash is reported by execute_process as a string such as
# "Segmentation fault", which never equals a number, so crashes fail the test.

foreach(required COMMAND EXPECTED_EXIT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "RunAndCheck: ${required} is not set")
    endif()
endforeach()

execute_process(
    COMMAND "${COMMAND}" ${ARGS}
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE  stderr)

if(NOT exit_code STREQUAL EXPECTED_EXIT)
    message(FATAL_ERROR
        "expected exit code ${EXPECTED_EXIT}, got '${exit_code}'\n"
        "stdout:\n${stdout}\nstderr:\n${stderr}")
endif()

if(DEFINED EXPECTED_STDERR AND NOT stderr MATCHES "${EXPECTED_STDERR}")
    message(FATAL_ERROR
        "stderr does not match '${EXPECTED_STDERR}'\nstderr:\n${stderr}")
endif()
