if(NOT DEFINED WINDEPLOYQT_EXECUTABLE OR WINDEPLOYQT_EXECUTABLE STREQUAL "")
    message(FATAL_ERROR "WINDEPLOYQT_EXECUTABLE is not set")
endif()

if(NOT DEFINED WINDEPLOYQT_TARGET_FILE OR WINDEPLOYQT_TARGET_FILE STREQUAL "")
    message(FATAL_ERROR "WINDEPLOYQT_TARGET_FILE is not set")
endif()

if(NOT DEFINED WINDEPLOYQT_LOCK_FILE OR WINDEPLOYQT_LOCK_FILE STREQUAL "")
    message(FATAL_ERROR "WINDEPLOYQT_LOCK_FILE is not set")
endif()

get_filename_component(_lock_dir "${WINDEPLOYQT_LOCK_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${_lock_dir}")

message(STATUS "[Deploy] Waiting for windeployqt lock: ${WINDEPLOYQT_TARGET_FILE}")
file(LOCK "${WINDEPLOYQT_LOCK_FILE}" GUARD PROCESS TIMEOUT 1800 RESULT_VARIABLE _lock_result)
if(NOT _lock_result STREQUAL "0")
    message(FATAL_ERROR "Failed to lock ${WINDEPLOYQT_LOCK_FILE}: ${_lock_result}")
endif()
message(STATUS "[Deploy] windeployqt lock acquired")

message(STATUS "[Deploy] Running windeployqt: ${WINDEPLOYQT_TARGET_FILE}")
execute_process(
    COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations --verbose 2 "${WINDEPLOYQT_TARGET_FILE}"
    COMMAND_ECHO STDOUT
    RESULT_VARIABLE _windeployqt_result
)

if(NOT _windeployqt_result EQUAL 0)
    message(FATAL_ERROR "windeployqt failed for ${WINDEPLOYQT_TARGET_FILE}: ${_windeployqt_result}")
endif()

message(STATUS "[Deploy] windeployqt finished")
