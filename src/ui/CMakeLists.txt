# The interface: theme, screens, dialogs and printing.
add_library(tcgui STATIC
    theme.cpp theme.h
    main_window.cpp main_window.h
    dialogs.cpp dialogs.h
    printing.cpp printing.h
    screens.h screens_basic.cpp screens_modern.cpp screens_commander.cpp
)
target_include_directories(tcgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcgui PUBLIC tcgcore Qt6::Widgets Qt6::PrintSupport Qt6::Svg)


set(TCG_ASSETS ${CMAKE_SOURCE_DIR}/assets/assets.qrc)

add_executable(TcgTournamentManager WIN32 ${CMAKE_SOURCE_DIR}/src/main.cpp ${TCG_ASSETS})
target_link_libraries(TcgTournamentManager PRIVATE tcgui)

add_executable(test_ui ${CMAKE_SOURCE_DIR}/tests/test_ui.cpp ${TCG_ASSETS})
target_link_libraries(test_ui PRIVATE tcgui Qt6::Test)
add_test(NAME ui COMMAND test_ui)
