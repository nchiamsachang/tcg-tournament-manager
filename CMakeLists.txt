cmake_minimum_required(VERSION 3.21)
project(TcgTournamentManager VERSION 0.2.0 LANGUAGES CXX)

# The version shown in the app and used to name the package.  The label marks an
# unfinished release; set it to "" for a final one.
set(TCG_VERSION_LABEL "preview")
set(TCG_VERSION "${PROJECT_VERSION}")
if(TCG_VERSION_LABEL)
    string(APPEND TCG_VERSION "-${TCG_VERSION_LABEL}")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${TCG_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql PrintSupport Svg Test)

if(MSVC)
    add_compile_options(/W3 /utf-8 /permissive-)
endif()

# Rules, pairing and persistence: no user interface.
add_library(tcgcore STATIC
    src/core/db.cpp src/core/db.h
    src/core/store.h
    src/core/tournaments.cpp src/core/tournaments.h
    src/core/players.cpp src/core/players.h
    src/core/swiss.cpp src/core/swiss.h
    src/core/round_clock.cpp src/core/round_clock.h
    src/core/prefs.cpp src/core/prefs.h
    src/core/round_status.cpp src/core/round_status.h
    src/core/commander.cpp src/core/commander.h
    src/core/commander_db.cpp src/core/commander_db.h
)
target_include_directories(tcgcore PUBLIC src/core)
target_compile_definitions(tcgcore PRIVATE TCG_VERSION="${TCG_VERSION}")
target_link_libraries(tcgcore PUBLIC Qt6::Core Qt6::Sql)

enable_testing()
add_executable(test_core tests/test_core.cpp)
target_compile_definitions(test_core PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
target_link_libraries(test_core PRIVATE tcgcore Qt6::Test)
add_test(NAME core COMMAND test_core)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/CMakeLists.txt")
    add_subdirectory(src/ui)
endif()
