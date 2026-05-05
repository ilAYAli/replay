cmake_minimum_required(VERSION 3.15)

project(replay)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

include_directories(/opt/homebrew/include/)

add_executable(replay replay.cpp validator.cpp)

find_package(fmt REQUIRED)
target_link_libraries(replay PRIVATE fmt::fmt-header-only)

string(ASCII 27 Esc)
set(COLOR_RESET "${Esc}[0m")
set(COLOR_CYAN "${Esc}[36m")
message(STATUS "${COLOR_CYAN}${PROJECT_NAME}${COLOR_RESET} build: ${CMAKE_BUILD_TYPE}")
message(STATUS "${COLOR_CYAN}${PROJECT_NAME}${COLOR_RESET} flags: ${CMAKE_CXX_FLAGS}")
get_target_property(LINK_LIBRARIES ${PROJECT_NAME} LINK_LIBRARIES)
message(STATUS "${COLOR_CYAN}${PROJECT_NAME}${COLOR_RESET} libs: ${LINK_LIBRARIES}")
