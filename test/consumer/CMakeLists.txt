# A game that consumes the installed package, as a game would: find_package only.
cmake_minimum_required(VERSION 3.16)
project(haiven_consumer LANGUAGES CXX)

set(HAIVEN_SDK_VERSION "0.1.0-preview.1")  # x-release-please-version
string(REGEX MATCH "^[0-9]+[.][0-9]+[.][0-9]+" HAIVEN_CMAKE_VERSION "${HAIVEN_SDK_VERSION}")
find_package(haiven "${HAIVEN_CMAKE_VERSION}" CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE haiven::haiven)
