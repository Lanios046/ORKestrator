include_guard(GLOBAL)

if(WIN32)
    get_target_property(_orkestrator_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_orkestrator_qt_bin "${_orkestrator_qmake}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt
        HINTS "${_orkestrator_qt_bin}"
        REQUIRED)
    set(WINDEPLOYQT_LOCK_FILE "${CMAKE_BINARY_DIR}/windeployqt.lock"
        CACHE FILEPATH "Lock file for serialized windeployqt runs")
    message(STATUS "[Deploy] Windeployqt path: ${WINDEPLOYQT_EXECUTABLE}")
endif()

function(add_locked_windeployqt TARGET_NAME)
    if(NOT WIN32)
        return()
    endif()

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DWINDEPLOYQT_EXECUTABLE:FILEPATH=${WINDEPLOYQT_EXECUTABLE}"
            "-DWINDEPLOYQT_TARGET_FILE:FILEPATH=$<TARGET_FILE:${TARGET_NAME}>"
            "-DWINDEPLOYQT_LOCK_FILE:FILEPATH=${WINDEPLOYQT_LOCK_FILE}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/run_windeployqt_locked.cmake"
        COMMENT "Deploying Qt runtime for ${TARGET_NAME}"
        USES_TERMINAL
        VERBATIM
    )
endfunction()