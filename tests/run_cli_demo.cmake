# Integration test driver: run `uapp-notes demo` against a freshly created
# database directory. Written as a CMake script so it needs no shell and works
# identically on Windows.
#
# Requires -DUAPP_CLI=<path to executable> -DUAPP_WORKDIR=<scratch dir>.

if(NOT DEFINED UAPP_CLI OR NOT DEFINED UAPP_WORKDIR)
    message(FATAL_ERROR "run_cli_demo.cmake requires UAPP_CLI and UAPP_WORKDIR")
endif()

# Start from a clean slate so the demo's assertions about note counts hold.
file(REMOVE_RECURSE "${UAPP_WORKDIR}")
file(MAKE_DIRECTORY "${UAPP_WORKDIR}")

set(_db "${UAPP_WORKDIR}/notes.db")

execute_process(
    COMMAND "${UAPP_CLI}" --db "${_db}" demo
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE  _err)

message(STATUS "${_out}")
if(NOT _err STREQUAL "")
    message(STATUS "stderr: ${_err}")
endif()

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "uapp-notes demo exited with ${_rc}")
endif()

# The demo created notes; a second process must still see them. This is the
# part a single in-process run cannot prove.
execute_process(
    COMMAND "${UAPP_CLI}" --db "${_db}" count
    RESULT_VARIABLE _rc2
    OUTPUT_VARIABLE _count
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT _rc2 EQUAL 0)
    message(FATAL_ERROR "uapp-notes count exited with ${_rc2}")
endif()
if(NOT _count STREQUAL "2")
    message(FATAL_ERROR "expected 2 persisted notes after the demo, found '${_count}'")
endif()

message(STATUS "cli_demo: ${_count} notes persisted and visible to a second process")
