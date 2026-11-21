add_library(orkestrator_core STATIC
    network/lmstudioclient.h network/lmstudioclient.cpp
    orchestration/protocol.h orchestration/protocol.cpp
    orchestration/orchestrator.h orchestration/orchestrator.cpp
    saving/sessionstore.h saving/sessionstore.cpp
    orchestration/taskexecutor.h orchestration/taskexecutor.cpp
)
target_include_directories(orkestrator_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(orkestrator_core PUBLIC Qt6::Core Qt6::Network PRIVATE spdlog::spdlog)

qt_add_executable(ORKestrator WIN32
    main/main.cpp main/mainwindow.h main/mainwindow.cpp
    main/applicationstyle.h main/applicationstyle.cpp
    ../resources/resources.qrc
)
target_link_libraries(ORKestrator PRIVATE orkestrator_core Qt6::Widgets spdlog::spdlog)
set_target_properties(ORKestrator PROPERTIES WIN32_EXECUTABLE TRUE MACOSX_BUNDLE TRUE)


if(WIN32)
    set(ORKESTRATOR_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../resources/icons/orkestrator.ico")
    configure_file(main/orkestrator.rc.in "${CMAKE_CURRENT_BINARY_DIR}/orkestrator.rc" @ONLY)
    target_sources(ORKestrator PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/orkestrator.rc")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/orkestrator.rc"
        PROPERTIES OBJECT_DEPENDS "${ORKESTRATOR_ICON_PATH}")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/locked_windeployqt.cmake")
add_locked_windeployqt(ORKestrator)
