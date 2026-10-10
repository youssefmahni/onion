cmake_minimum_required(VERSION 3.20)

project(onion_engine VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
include(FetchContent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ONION_ENABLE_OPENCL "Enable the optional OpenCL GPU layer" ON)
option(ONION_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(ONION_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(ONION_ENABLE_TSAN "Build with ThreadSanitizer" OFF)

if((ONION_ENABLE_ASAN OR ONION_ENABLE_UBSAN OR ONION_ENABLE_TSAN) AND MSVC)
  message(FATAL_ERROR "The configured sanitizer jobs require a GCC or Clang toolchain")
endif()
if(ONION_ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
if(ONION_ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
endif()
if(ONION_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

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

onion_add_compiled_module(platform src/platform/platform.cpp)
if(WIN32)
  # platform is static, so consumers must receive the Windows API import libs.
  target_link_libraries(platform PUBLIC Crypt32 Iphlpapi)
  target_compile_definitions(platform PRIVATE
    _WIN32_WINNT=0x0A00
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
endif()

# Data layer
onion_add_module(log)
onion_add_module(store)
if(WIN32)
  # Windows is the sole platform that builds the pinned curl source.  Schannel
  # keeps certificate validation on the Windows trust store.
  set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
  set(CURL_USE_SECTRANSP OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    curl
    URL https://github.com/curl/curl/releases/download/curl-8_11_1/curl-8.11.1.tar.xz
    URL_HASH SHA256=c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(curl)
  set(ONION_CURL_TARGET libcurl_static)
else()
  find_package(CURL 7.66 REQUIRED)
  set(ONION_CURL_TARGET CURL::libcurl)
endif()
find_package(Threads REQUIRED)
onion_add_compiled_module(llm src/llm/limiter.cpp src/llm/client.cpp)
target_link_libraries(llm PUBLIC ${ONION_CURL_TARGET} Threads::Threads)
if(WIN32)
  target_compile_definitions(llm PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
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
    monocypher
    URL https://monocypher.org/download/monocypher-4.0.2.tar.gz
    URL_HASH SHA256=38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(monocypher)
  add_library(monocypher STATIC
    "${monocypher_SOURCE_DIR}/src/monocypher.c"
    "${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c")
  target_include_directories(monocypher PUBLIC
    "${monocypher_SOURCE_DIR}/src"
    "${monocypher_SOURCE_DIR}/src/optional")

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

  add_executable(onion_platform_test tests/unit/platform_test.cpp)
  target_link_libraries(onion_platform_test PRIVATE onion::platform GTest::gtest_main)
  gtest_discover_tests(onion_platform_test)

  add_executable(onion_limiter_test tests/unit/limiter_test.cpp)
  target_link_libraries(onion_limiter_test PRIVATE onion::llm GTest::gtest_main)
  gtest_discover_tests(onion_limiter_test)

  add_executable(onion_llm_test tests/unit/llm_test.cpp)
  target_link_libraries(onion_llm_test PRIVATE onion::llm GTest::gtest_main)
  gtest_discover_tests(onion_llm_test)

  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME fake_deepseek.smoke COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/fake_deepseek_test.py")
  add_test(NAME fake_deepseek.concurrent_1000 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/fake_deepseek_load_test.py")
  set_tests_properties(fake_deepseek.concurrent_1000 PROPERTIES TIMEOUT 90)
  add_test(NAME llm.closed_port_1000 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/llm_client_failure_test.py" "--mode" "closed-port" "$<TARGET_FILE:data_depot>")
  add_test(NAME llm.reset_connections_1000 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/llm_client_failure_test.py" "--mode" "reset" "$<TARGET_FILE:data_depot>")
  add_test(NAME llm.timeout_1000 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/llm_client_failure_test.py" "--mode" "timeout" "$<TARGET_FILE:data_depot>")
  add_test(NAME llm.auth_401 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/llm_client_failure_test.py" "--mode" "auth" "$<TARGET_FILE:data_depot>")
  add_test(NAME llm.balance_402 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/llm_client_failure_test.py" "--mode" "balance" "$<TARGET_FILE:data_depot>")
  add_test(NAME llm.limiter_traces COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/limiter_trace_test.py" "$<TARGET_FILE:data_depot>")
  set_tests_properties(llm.closed_port_1000 llm.reset_connections_1000 llm.timeout_1000 llm.auth_401 llm.balance_402 PROPERTIES TIMEOUT 90)

  add_executable(onion_golden_test tests/golden/golden_test.cpp)
  target_link_libraries(onion_golden_test PRIVATE monocypher GTest::gtest_main)
  target_compile_definitions(onion_golden_test PRIVATE
    ONION_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/expected")
  gtest_discover_tests(onion_golden_test)
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

add_executable(data_depot src/cli/main.cpp)
target_link_libraries(data_depot PRIVATE onion::llm)
