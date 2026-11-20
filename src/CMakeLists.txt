add_library(orkestrator_core STATIC
    network/lmstudioclient.h network/lmstudioclient.cpp
    orchestration/protocol.h orchestration/protocol.cpp
    orchestration/orchestrator.h orchestration/orchestrator.cpp
    saving/sessionstore.h saving/sessionstore.cpp
)
target_include_directories(orkestrator_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(orkestrator_core PUBLIC Qt6::Core Qt6::Network PRIVATE spdlog::spdlog)

qt_add_executable(ORKestrator WIN32
    main/main.cpp main/mainwindow.h main/mainwindow.cpp
    main/applicationstyle.h main/applicationstyle.cpp
)
target_link_libraries(ORKestrator PRIVATE orkestrator_core Qt6::Widgets spdlog::spdlog)
set_target_properties(ORKestrator PROPERTIES WIN32_EXECUTABLE TRUE MACOSX_BUNDLE TRUE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/locked_windeployqt.cmake")
add_locked_windeployqt(ORKestrator)
