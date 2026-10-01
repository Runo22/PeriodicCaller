cmake_minimum_required(VERSION 3.20)
project(PeriodicCaller VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "PeriodicCaller targets Windows only.")
endif()

option(PERIODIC_CALLER_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
option(PERIODIC_CALLER_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})

add_library(periodic_caller src/PeriodicCaller.cpp)
add_library(periodic_caller::periodic_caller ALIAS periodic_caller)
target_include_directories(periodic_caller PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(periodic_caller PUBLIC cxx_std_23)
target_link_libraries(periodic_caller PRIVATE winmm)
if(MSVC)
    target_compile_options(periodic_caller PRIVATE /W4 /permissive-)
else()
    target_compile_options(periodic_caller PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PERIODIC_CALLER_BUILD_EXAMPLES)
    add_executable(periodic_caller_example examples/basic.cpp)
    target_link_libraries(periodic_caller_example PRIVATE periodic_caller)
endif()

if(PERIODIC_CALLER_BUILD_TESTS)
    enable_testing()
    add_executable(periodic_caller_tests tests/tests.cpp)
    target_link_libraries(periodic_caller_tests PRIVATE periodic_caller)
    add_test(NAME periodic_caller_tests COMMAND periodic_caller_tests)
endif()
