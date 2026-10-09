cmake_minimum_required(VERSION 3.20)

project(onion_engine VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(FetchContent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ONION_ENABLE_OPENCL "Enable the optional OpenCL GPU layer" ON)

function(onion_add_module module)
  add_library(${module} INTERFACE)
  add_library(onion::${module} ALIAS ${module})
  target_include_directories(${module} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

  if(MSVC)
    target_compile_options(${module} INTERFACE /W4 /permissive-)
  else()
    target_compile_options(${module} INTERFACE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

function(onion_add_compiled_module module)
  add_library(${module} STATIC ${ARGN})
  add_library(onion::${module} ALIAS ${module})
  target_include_directories(${module} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

  if(MSVC)
    target_compile_options(${module} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${module} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# Base layer
onion_add_compiled_module(core src/core/depot_core.cpp)

if(ONION_ENABLE_OPENCL)
  find_path(ONION_OPENCL_INCLUDE_DIR CL/cl.h
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/OpenCL-Headers")
  find_library(ONION_OPENCL_LIBRARY NAMES OpenCL libOpenCL.so.1)
endif()

if(ONION_OPENCL_INCLUDE_DIR AND ONION_OPENCL_LIBRARY)
  onion_add_compiled_module(gpu src/gpu/depot_gpu.cpp)
  target_include_directories(gpu PUBLIC "${ONION_OPENCL_INCLUDE_DIR}")
  target_link_libraries(gpu PUBLIC onion::core "${ONION_OPENCL_LIBRARY}")
  target_compile_definitions(gpu PUBLIC ONION_HAVE_OPENCL=1)
  set(ONION_GPU_AVAILABLE ON)
  message(STATUS "onion_gpu: enabled (${ONION_OPENCL_LIBRARY})")
else()
  onion_add_module(gpu)
  message(STATUS "onion_gpu: skipped (OpenCL loader or headers not found)")
endif()

onion_add_module(platform)

# Data layer
onion_add_module(log)
onion_add_module(store)
onion_add_module(llm)
target_link_libraries(log INTERFACE onion::core onion::platform)
target_link_libraries(store INTERFACE onion::core onion::platform)
target_link_libraries(llm INTERFACE onion::core onion::platform)

# Fleet layer
onion_add_module(sync)
onion_add_module(swim)
onion_add_module(profile)
target_link_libraries(sync INTERFACE onion::log onion::store onion::platform)
target_link_libraries(swim INTERFACE onion::core onion::platform)
target_link_libraries(profile INTERFACE onion::core onion::gpu onion::platform)

# Engine layer
onion_add_module(gate)
onion_add_module(sched)
onion_add_module(state)
target_link_libraries(gate INTERFACE onion::core onion::llm)
target_link_libraries(sched INTERFACE onion::core onion::profile)
target_link_libraries(state INTERFACE onion::core onion::log onion::store)

# Interface layer
onion_add_module(api)
onion_add_module(cli)
target_link_libraries(api INTERFACE onion::state)
target_link_libraries(cli INTERFACE onion::api onion::gate onion::sched onion::state)

add_executable(onion_smoke tests/unit/smoke_test.cpp)
target_link_libraries(onion_smoke PRIVATE onion::cli)

if(BUILD_TESTING)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_test(NAME onion.smoke COMMAND onion_smoke)
  set_tests_properties(onion.smoke PROPERTIES
    ENVIRONMENT "POCL_CACHE_DIR=${CMAKE_BINARY_DIR}/pocl-cache")

  add_executable(onion_core_test tests/unit/core_test.cpp)
  target_link_libraries(onion_core_test PRIVATE onion::core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(onion_core_test)
  if(ONION_GPU_AVAILABLE)
    add_executable(onion_gpu_test tests/unit/gpu_test.cpp)
    target_link_libraries(onion_gpu_test PRIVATE onion::gpu GTest::gtest_main)
    gtest_discover_tests(onion_gpu_test
      PROPERTIES ENVIRONMENT "POCL_CACHE_DIR=${CMAKE_BINARY_DIR}/pocl-cache")
  else()
    foreach(gpu_test Enumeration VectorAddKnownAnswer BrokenKernelFailsCleanly
        SimParityRandomPairs SimParityMatrix SimParityMultiDeviceMatrix
        SimBatchMultiDeviceSplit SimBoundariesAndFallback)
      add_test(NAME Gpu.${gpu_test} COMMAND ${CMAKE_COMMAND} -E false)
      set_tests_properties(Gpu.${gpu_test} PROPERTIES SKIP_RETURN_CODE 1)
    endforeach()
  endif()
endif()
