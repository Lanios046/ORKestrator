add_executable(orchestration_tests orchestration_tests.cpp fakelmstudioserver.h)
target_link_libraries(orchestration_tests PRIVATE orkestrator_core Qt6::Test)
add_test(NAME orchestration_tests COMMAND orchestration_tests)

add_executable(gui_tests gui_tests.cpp
    ../src/main/mainwindow.h ../src/main/mainwindow.cpp
    ../src/pipeline/agentgraph.h
    ../src/main/applicationstyle.h ../src/main/applicationstyle.cpp
)
target_link_libraries(gui_tests PRIVATE orkestrator_core Qt6::Widgets Qt6::Test QtNodes::QtNodes)
add_test(NAME gui_tests COMMAND gui_tests)
set_tests_properties(gui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(lmstudio_smoke lmstudio_smoke.cpp)
target_link_libraries(lmstudio_smoke PRIVATE orkestrator_core)
if(WIN32)
    get_target_property(_qt_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qt_qmake}" DIRECTORY)
    set_tests_properties(orchestration_tests gui_tests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qt_bin}")
endif()
