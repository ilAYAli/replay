cmake_minimum_required(VERSION 3.15)

project(replay)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(/opt/homebrew/include/)

set(ENYO_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../enyo" CACHE PATH "Path to Enyo source tree")

find_package(fmt REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(enyo_chess STATIC
    "${ENYO_SOURCE_DIR}/src/fen.cpp"
    "${ENYO_SOURCE_DIR}/src/board.cpp"
    "${ENYO_SOURCE_DIR}/magic/magic.cpp"
    "${ENYO_SOURCE_DIR}/src/pgn.cpp"
    "${ENYO_SOURCE_DIR}/src/zobrist.cpp"
)
target_include_directories(enyo_chess PUBLIC
    "${ENYO_SOURCE_DIR}/src"
    "${ENYO_SOURCE_DIR}"
)
target_link_libraries(enyo_chess PUBLIC
    fmt::fmt-header-only
    nlohmann_json::nlohmann_json
)

add_executable(replay
    analysis_cache.cpp
    engine_process.cpp
    lichess_analysis.cpp
    replay.cpp
)
set_target_properties(replay PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(replay PRIVATE fmt::fmt-header-only enyo_chess)

string(ASCII 27 Esc)
set(COLOR_RESET "${Esc}[0m")
set(COLOR_CYAN "${Esc}[36m")
message(STATUS "${COLOR_CYAN}${PROJECT_NAME}${COLOR_RESET} build: ${CMAKE_BUILD_TYPE}")
message(STATUS "${COLOR_CYAN}${PROJECT_NAME}${COLOR_RESET} flags: ${CMAKE_CXX_FLAGS}")
get_target_property(LINK_LIBRARIES ${PROJECT_NAME} LINK_LIBRARIES)
message(STATUS "${COLOR_CYAN}${PROJECT_NAME}${COLOR_RESET} libs: ${LINK_LIBRARIES}")
