cmake_minimum_required(VERSION 3.20)

project(onion_engine VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

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

# Base layer
onion_add_module(core)
onion_add_module(gpu)
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
  add_test(NAME onion.smoke COMMAND onion_smoke)
  set_tests_properties(onion.smoke PROPERTIES
    ENVIRONMENT "POCL_CACHE_DIR=${CMAKE_BINARY_DIR}/pocl-cache")
endif()
