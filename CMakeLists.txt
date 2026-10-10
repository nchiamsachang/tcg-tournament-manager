cmake_minimum_required(VERSION 3.21)
project(TcgTournamentManager VERSION 0.2.1 LANGUAGES CXX)

# The version shown in the app and used to name the package.  The label marks an
# unfinished release; set it to "" for a final one.
set(TCG_VERSION_LABEL "preview.3")
set(TCG_VERSION "${PROJECT_VERSION}")
if(TCG_VERSION_LABEL)
    string(APPEND TCG_VERSION "-${TCG_VERSION_LABEL}")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${TCG_VERSION}")

# The exact build: the Git commit it was made from.  When files differ from that commit the
# identifier says so and carries the time of the build, so two such builds can be told
# apart.  Without Git (a source archive, Git not installed) it is "no-git": nothing is made up.
set(TCG_BUILD "no-git")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=8 HEAD
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE TCG_COMMIT
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE TCG_GIT_RESULT)
    if(TCG_GIT_RESULT EQUAL 0 AND TCG_COMMIT)
        set(TCG_BUILD "${TCG_COMMIT}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain
                        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE TCG_CHANGES
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(TCG_CHANGES)
            string(TIMESTAMP TCG_BUILD_TIME "%Y%m%d-%H%M")
            string(APPEND TCG_BUILD "-modified-${TCG_BUILD_TIME}")
        endif()
    endif()
endif()
file(WRITE "${CMAKE_BINARY_DIR}/build_id.txt" "${TCG_BUILD}")
message(STATUS "TCG Tournament Manager ${TCG_VERSION}, build ${TCG_BUILD}")

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
    src/core/applog.cpp src/core/applog.h
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
# only the file that holds it, so a new build identifier does not recompile everything
set_source_files_properties(src/core/prefs.cpp PROPERTIES COMPILE_DEFINITIONS "TCG_BUILD=\"${TCG_BUILD}\"")
target_link_libraries(tcgcore PUBLIC Qt6::Core Qt6::Sql)

enable_testing()
add_executable(test_core tests/test_core.cpp)
target_compile_definitions(test_core PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
target_link_libraries(test_core PRIVATE tcgcore Qt6::Test)
add_test(NAME core COMMAND test_core)

# Hand-worked rule examples and pairing checks; see docs/qa.
add_executable(test_rules tests/test_rules.cpp)
target_link_libraries(test_rules PRIVATE tcgcore Qt6::Test)
add_test(NAME rules COMMAND test_rules)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/CMakeLists.txt")
    add_subdirectory(src/ui)
endif()
