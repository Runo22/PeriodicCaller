cmake_minimum_required(VERSION 3.21)
project(PeriodicCaller VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "PeriodicCaller supports MSVC (Visual Studio 2022 17.2+ / 2026) only.")
endif()

option(PERIODIC_CALLER_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
option(PERIODIC_CALLER_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(PERIODIC_CALLER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(PERIODIC_CALLER_SHARED "Build periodic_caller as a DLL (one shared scheduler per process)" OFF)

# Shared MSVC settings for every target in this project.
function(periodic_caller_configure target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(PERIODIC_CALLER_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE /WX)
    endif()
endfunction()

if(PERIODIC_CALLER_SHARED)
    add_library(periodic_caller SHARED src/PeriodicCaller.cpp)
    target_compile_definitions(periodic_caller
        PRIVATE PERIODIC_CALLER_EXPORTS
        INTERFACE PERIODIC_CALLER_IMPORTS)
else()
    add_library(periodic_caller STATIC src/PeriodicCaller.cpp)
endif()
add_library(periodic_caller::periodic_caller ALIAS periodic_caller)
target_include_directories(periodic_caller PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
# std::move_only_function in the public header needs C++23 for consumers too.
target_compile_features(periodic_caller PUBLIC cxx_std_23)
target_link_libraries(periodic_caller PRIVATE winmm)
periodic_caller_configure(periodic_caller)

if(PERIODIC_CALLER_BUILD_EXAMPLES)
    add_executable(periodic_caller_example examples/basic.cpp)
    target_link_libraries(periodic_caller_example PRIVATE periodic_caller)
    periodic_caller_configure(periodic_caller_example)
endif()

if(PERIODIC_CALLER_BUILD_TESTS)
    enable_testing()
    add_executable(periodic_caller_tests tests/tests.cpp)
    target_link_libraries(periodic_caller_tests PRIVATE periodic_caller)
    periodic_caller_configure(periodic_caller_tests)
    add_test(NAME periodic_caller_tests COMMAND periodic_caller_tests)

    if(PERIODIC_CALLER_SHARED)
        # Simulates a plugin DLL using the shared scheduler and being unloaded.
        add_library(periodic_caller_test_plugin MODULE tests/test_plugin.cpp)
        target_link_libraries(periodic_caller_test_plugin PRIVATE periodic_caller)
        periodic_caller_configure(periodic_caller_test_plugin)

        add_executable(periodic_caller_plugin_tests tests/plugin_tests.cpp)
        target_link_libraries(periodic_caller_plugin_tests PRIVATE periodic_caller)
        periodic_caller_configure(periodic_caller_plugin_tests)
        add_dependencies(periodic_caller_plugin_tests periodic_caller_test_plugin)
        add_test(NAME periodic_caller_plugin_tests
                 COMMAND periodic_caller_plugin_tests $<TARGET_FILE:periodic_caller_test_plugin>)
    endif()
endif()
