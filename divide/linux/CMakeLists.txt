cmake_minimum_required(VERSION 3.15)

project(divide)

string(ASCII 27 Esc)
set(COLOR_BLUE "${Esc}[38;5;4m")
set(COLOR_RESET "${Esc}[0m")
message(STATUS "${COLOR_BLUE}${PROJECT_NAME}${COLOR_RESET}: build: \"${CMAKE_BUILD_TYPE}\" cxx flags: \"${CMAKE_CXX_FLAGS}\"")


set(CMAKE_CXX_STANDARD 20)
#set(CMAKE_CXX_FLAGS "-O3 -march=native -flto")
set(CMAKE_CXX_FLAGS "-O1 -g")
set(CMAKE_VERBOSE_MAKEFILE ON)

find_library(PROFILER_LIBRARY NAMES profiler HINTS /opt/homebrew/lib)
if(NOT PROFILER_LIBRARY)
    message(FATAL_ERROR "Google CPU Profiler library not found.")
endif()


# Add the source files to the project
set(SOURCE_FILES divide.cpp ../../fen.cpp ../../perft.cpp ../../engine.cpp)

# Add include directories
include_directories(../../ /opt/homebrew/include/)

# Create the executable
add_executable(divide ${SOURCE_FILES})

# target_link_libraries(divide PRIVATE ${PROFILER_LIBRARY})

# Find and link the fmt library
find_library(FMT_LIBRARY fmt)
if (FMT_LIBRARY)
    target_link_libraries(divide PRIVATE ${FMT_LIBRARY})
else()
    message(FATAL_ERROR "fmt library not found")
endif()
