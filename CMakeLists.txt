cmake_minimum_required(VERSION 3.22)

# Build for both Apple Silicon and Intel by default, and run on older macOS versions.
# (Must be set before project().)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
endif()

project(ToneGenie VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Link the MSVC runtime statically so the plugin has no redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(TONEGENIE_BUILD_PLUGIN "Build the audio plugin (downloads JUCE)" ON)
option(TONEGENIE_BUILD_TESTS "Build the test suite" ON)
option(TONEGENIE_BUILD_TOOLS "Build the command-line tool" ON)
option(TONEGENIE_BUILD_CLAP "Also build a CLAP plugin (downloads clap-juce-extensions)" ON)
option(TONEGENIE_COPY_AFTER_BUILD "Install the plugins into the user's plugin folders after building" OFF)

#===============================================================================
# DSP core: plain C++17, no dependencies.
add_library(tonegenie_core STATIC
    core/src/Analysis.cpp
    core/src/Bands.cpp
    core/src/Fft.cpp
    core/src/FrontEnd.cpp
    core/src/MatchEq.cpp
    core/src/Reference.cpp
    core/src/SyntheticGuitar.cpp
    core/src/ToneEngine.cpp
    core/src/ToneFitter.cpp)

target_include_directories(tonegenie_core PUBLIC core/include)

find_package(Threads REQUIRED)
target_link_libraries(tonegenie_core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(tonegenie_core PRIVATE /W4 /fp:fast)
    target_compile_definitions(tonegenie_core PUBLIC _USE_MATH_DEFINES NOMINMAX)
else()
    target_compile_options(tonegenie_core PRIVATE -Wall -Wextra -ffast-math)
endif()

#===============================================================================
if(TONEGENIE_BUILD_TOOLS OR TONEGENIE_BUILD_TESTS)
    add_library(tonegenie_wav STATIC tools/WavIo.cpp)
    target_include_directories(tonegenie_wav PUBLIC tools)
endif()

if(TONEGENIE_BUILD_TOOLS)
    add_executable(tonegenie-cli tools/tonegenie_cli.cpp)
    target_link_libraries(tonegenie-cli PRIVATE tonegenie_core tonegenie_wav)
endif()

if(TONEGENIE_BUILD_TESTS)
    enable_testing()
    add_executable(tonegenie_tests tests/Tests.cpp)
    target_link_libraries(tonegenie_tests PRIVATE tonegenie_core tonegenie_wav)
    add_test(NAME tonegenie_tests COMMAND tonegenie_tests)
endif()

#===============================================================================
if(TONEGENIE_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()
