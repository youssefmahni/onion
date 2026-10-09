cmake_minimum_required(VERSION 3.16)
project(data_depot CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(DEPOT_ENABLE_COVERAGE "Build with gcov coverage" OFF)
if(DEPOT_ENABLE_COVERAGE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

find_package(Threads REQUIRED)

add_library(depot_core STATIC depot_core.cpp)
target_include_directories(depot_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(depot_core PUBLIC Threads::Threads)

add_library(depot_io STATIC depot_io.cpp)
target_link_libraries(depot_io PUBLIC depot_core)

add_executable(data_depot main.cpp)
target_link_libraries(data_depot depot_io)

# ---- optional OpenCL GPU layer (auto-skipped when the loader is absent) ----
option(DEPOT_OPENCL "Build the OpenCL GPU layer" ON)
if(DEPOT_OPENCL)
    find_library(OpenCL_LIBRARY NAMES OpenCL libOpenCL.so.1)
    if(OpenCL_LIBRARY)
        add_library(depot_gpu STATIC depot_gpu.cpp)
        target_include_directories(depot_gpu PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/OpenCL-Headers)
        target_link_libraries(depot_gpu PUBLIC ${OpenCL_LIBRARY} depot_core)
        target_compile_definitions(depot_gpu PUBLIC DEPOT_HAVE_OPENCL=1)
        target_link_libraries(data_depot depot_gpu)
        target_link_libraries(depot_io PUBLIC depot_gpu)
        message(STATUS "depot_gpu: enabled (${OpenCL_LIBRARY})")
    else()
        message(STATUS "depot_gpu: skipped (OpenCL loader not found)")
    endif()
endif()

# ---- optional native OpenGL TUI (desktop only; auto-skipped when X11/GL absent) ----
option(ONION_GL "Build the OpenGL TUI (requires X11 + GL headers)" ON)
if(ONION_GL)
    find_path(X11_INCLUDE_DIR X11/Xlib.h)
    find_path(GL_INCLUDE_DIR GL/gl.h)
    find_library(X11_LIBRARY X11)
    find_library(GL_LIBRARY GL)
    if(X11_INCLUDE_DIR AND GL_INCLUDE_DIR AND X11_LIBRARY AND GL_LIBRARY)
        add_executable(onion_gl onion_gl.cpp)
        target_include_directories(onion_gl PRIVATE
            ${X11_INCLUDE_DIR} ${GL_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
        target_link_libraries(onion_gl Threads::Threads ${X11_LIBRARY} ${GL_LIBRARY})
        # optional EGL -> terminal mode (offscreen render + sixel/blocks in the terminal)
        find_path(EGL_INCLUDE_DIR EGL/egl.h)
        find_library(EGL_LIBRARY EGL)
        if(EGL_INCLUDE_DIR AND EGL_LIBRARY)
            target_include_directories(onion_gl PRIVATE ${EGL_INCLUDE_DIR})
            target_link_libraries(onion_gl ${EGL_LIBRARY})
            target_compile_definitions(onion_gl PRIVATE ONION_HAVE_EGL=1)
            message(STATUS "onion_gl: EGL found -> terminal mode (--term) enabled")
        else()
            message(STATUS "onion_gl: EGL not found -> terminal mode disabled")
        endif()
        message(STATUS "onion_gl: enabled (X11 + GL found)")
    else()
        message(STATUS "onion_gl: skipped (X11/GL headers or libs not found)")
    endif()
endif()

enable_testing()

include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(depot_core_test depot_core_test.cpp)
target_link_libraries(depot_core_test depot_io gtest_main)

include(GoogleTest)
gtest_discover_tests(depot_core_test)

if(TARGET depot_gpu)
    add_executable(depot_gpu_test depot_gpu_test.cpp)
    target_link_libraries(depot_gpu_test depot_gpu gtest_main)
    gtest_discover_tests(depot_gpu_test)
endif()

# ---- optional WASM module (dashboard policy) ----
find_program(EMXX_FOUND em++)
if(EMXX_FOUND)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/depot_core_wasm.js
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}
        COMMAND ${EMXX_FOUND} -O3 -std=c++17 ${CMAKE_CURRENT_SOURCE_DIR}/depot_core.cpp
                -I${CMAKE_CURRENT_SOURCE_DIR}
                -s EXPORTED_FUNCTIONS='["_wasm_policy_target"]'
                -s EXPORTED_RUNTIME_METHODS='["cwrap"]'
                -s MODULARIZE=1 -s EXPORT_NAME=createDepotModule
                -s SINGLE_FILE=1 -s ENVIRONMENT=web,node -s ALLOW_MEMORY_GROWTH=1
                -o ${CMAKE_BINARY_DIR}/depot_core_wasm.js
        DEPENDS depot_core.cpp depot_core.hpp
        COMMENT "Building WASM policy module (Emscripten)")
    add_custom_target(depot_core_wasm ALL DEPENDS ${CMAKE_BINARY_DIR}/depot_core_wasm.js)
endif()
