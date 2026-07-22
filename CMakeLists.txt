cmake_minimum_required(VERSION 3.20)
project(NanoXGen VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

option(NANOXGEN_BUILD_TESTS "Build NanoXGen tests" ON)
option(NANOXGEN_BUILD_CALIBRATION
  "Build Autodesk XGen calibration/oracle tools (requires a licensed XGen runtime)" OFF)
option(NANOXGEN_ENABLE_CUDA "Build the CUDA generation kernel" OFF)
option(NANOXGEN_NATIVE_ARCH "Tune CPU code for the build host (non-portable binary)" OFF)
option(NANOXGEN_FAST_MATH "Enable relaxed FP math; incompatible with strict parity" OFF)
option(NANOXGEN_ENABLE_IPO "Enable interprocedural optimization when supported" OFF)
set(NANOXGEN_SIMD_WIDTH "" CACHE STRING
  "Preferred x86 SIMD width for GCC/Clang: empty, 128, 256, or 512")
set(NANOXGEN_COMPAT_LIB_DIR "" CACHE PATH
  "Optional compatibility libraries used only by Autodesk calibration tools")
set_property(CACHE NANOXGEN_SIMD_WIDTH PROPERTY STRINGS "" 128 256 512)

if(NANOXGEN_ENABLE_IPO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT NANOXGEN_IPO_SUPPORTED OUTPUT NANOXGEN_IPO_ERROR)
  if(NANOXGEN_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  else()
    message(FATAL_ERROR "IPO requested but unsupported: ${NANOXGEN_IPO_ERROR}")
  endif()
endif()

add_library(nanoxgen_build_options INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nanoxgen_build_options INTERFACE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:-O3>)
  if(NANOXGEN_NATIVE_ARCH)
    target_compile_options(nanoxgen_build_options INTERFACE
      "$<$<COMPILE_LANGUAGE:CXX>:-march=native;-mtune=native>")
  endif()
  if(NANOXGEN_FAST_MATH)
    target_compile_options(nanoxgen_build_options INTERFACE
      $<$<COMPILE_LANGUAGE:CXX>:-ffast-math>)
  endif()
  if(NOT NANOXGEN_SIMD_WIDTH STREQUAL "")
    if(NOT NANOXGEN_SIMD_WIDTH MATCHES "^(128|256|512)$")
      message(FATAL_ERROR "NANOXGEN_SIMD_WIDTH must be empty, 128, 256, or 512")
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
      target_compile_options(nanoxgen_build_options INTERFACE
        "$<$<COMPILE_LANGUAGE:CXX>:-mprefer-vector-width=${NANOXGEN_SIMD_WIDTH}>")
    else()
      message(FATAL_ERROR "NANOXGEN_SIMD_WIDTH is only valid for x86 targets")
    endif()
  endif()
elseif(MSVC)
  if(NANOXGEN_NATIVE_ARCH)
    target_compile_options(nanoxgen_build_options INTERFACE
      $<$<COMPILE_LANGUAGE:CXX>:/arch:AVX2>)
  endif()
  if(NANOXGEN_FAST_MATH)
    target_compile_options(nanoxgen_build_options INTERFACE
      $<$<COMPILE_LANGUAGE:CXX>:/fp:fast>)
  endif()
endif()
if(NANOXGEN_FAST_MATH)
  message(WARNING "NANOXGEN_FAST_MATH changes FP/ULP behavior; do not use it for strict XGen parity")
endif()

add_library(nanoxgen STATIC
  src/asset.cpp
  src/curve_cache.cpp
  src/curve_payload.cpp
  src/xgen.cpp
  src/xgen_package.cpp)
add_library(NanoXGen::nanoxgen ALIAS nanoxgen)
target_include_directories(nanoxgen PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(nanoxgen PUBLIC cxx_std_20)
target_link_libraries(nanoxgen PUBLIC Threads::Threads ZLIB::ZLIB)
target_link_libraries(nanoxgen PRIVATE $<BUILD_INTERFACE:nanoxgen_build_options>)
set_target_properties(nanoxgen PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nanoxgen PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
  target_compile_options(nanoxgen PRIVATE /W4)
endif()

add_executable(nanoxgen_demo tools/demo.cpp)
target_link_libraries(nanoxgen_demo PRIVATE nanoxgen)

add_executable(nanoxgen_benchmark tools/benchmark.cpp)
target_link_libraries(nanoxgen_benchmark PRIVATE nanoxgen)

add_executable(nanoxgen_cache_benchmark tools/cache_benchmark.cpp)
target_link_libraries(nanoxgen_cache_benchmark PRIVATE nanoxgen)

add_executable(nanoxgen_precision tools/precision.cpp)
target_link_libraries(nanoxgen_precision PRIVATE nanoxgen)

add_executable(nanoxgen_xgen_inspect tools/xgen_inspect.cpp)
target_link_libraries(nanoxgen_xgen_inspect PRIVATE nanoxgen)

add_executable(nanoxgen_xgen_process tools/xgen_process.cpp)
target_link_libraries(nanoxgen_xgen_process PRIVATE nanoxgen)

add_executable(nanoxgen_xgen_cache tools/xgen_to_nxc.cpp)
target_link_libraries(nanoxgen_xgen_cache PRIVATE nanoxgen)

add_executable(nanoxgen_xgen_read_benchmark tools/xgen_read_benchmark.cpp)
target_link_libraries(nanoxgen_xgen_read_benchmark PRIVATE nanoxgen)

add_executable(nanoxgen_xgen_package tools/xgen_package.cpp)
target_link_libraries(nanoxgen_xgen_package PRIVATE nanoxgen)

install(TARGETS nanoxgen
  EXPORT NanoXGenTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS
  nanoxgen_demo
  nanoxgen_benchmark
  nanoxgen_cache_benchmark
  nanoxgen_precision
  nanoxgen_xgen_inspect
  nanoxgen_xgen_process
  nanoxgen_xgen_cache
  nanoxgen_xgen_read_benchmark
  nanoxgen_xgen_package
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(EXPORT NanoXGenTargets
  FILE NanoXGenTargets.cmake
  NAMESPACE NanoXGen::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NanoXGen)
configure_package_config_file(
  cmake/NanoXGenConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/NanoXGenConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NanoXGen)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/NanoXGenConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/NanoXGenConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/NanoXGenConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NanoXGen)

if(NANOXGEN_BUILD_TESTS)
  enable_testing()
  add_executable(nanoxgen_tests tests/test_main.cpp)
  target_link_libraries(nanoxgen_tests PRIVATE nanoxgen)
  add_test(NAME nanoxgen_tests COMMAND nanoxgen_tests)
  add_executable(nanoxgen_package_tests tests/package_test.cpp)
  target_link_libraries(nanoxgen_package_tests PRIVATE nanoxgen)
  add_test(NAME nanoxgen_package_tests COMMAND nanoxgen_package_tests)
endif()

if(NANOXGEN_ENABLE_CUDA)
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
  add_library(nanoxgen_cuda STATIC src/cuda/generate.cu)
  target_include_directories(nanoxgen_cuda PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(nanoxgen_cuda PUBLIC cuda_std_17)
  target_link_libraries(nanoxgen_cuda PUBLIC nanoxgen CUDA::cudart)
  set_target_properties(nanoxgen_cuda PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON)
  if(NANOXGEN_FAST_MATH)
    target_compile_options(nanoxgen_cuda PRIVATE --use_fast_math)
  endif()

  add_executable(nanoxgen_cuda_benchmark tools/cuda_benchmark.cpp)
  target_link_libraries(nanoxgen_cuda_benchmark PRIVATE nanoxgen_cuda)

  if(NANOXGEN_BUILD_TESTS)
    add_executable(nanoxgen_cuda_tests tests/cuda_test.cpp)
    target_link_libraries(nanoxgen_cuda_tests PRIVATE nanoxgen_cuda)
    add_test(NAME nanoxgen_cuda_tests COMMAND nanoxgen_cuda_tests)
    set_tests_properties(nanoxgen_cuda_tests PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

if(NANOXGEN_BUILD_CALIBRATION)
  list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
  find_package(XGen REQUIRED)
  add_executable(nanoxgen_xgen_probe tools/xgen_probe.cpp)
  target_link_libraries(nanoxgen_xgen_probe PRIVATE XGen::XGen)
  target_compile_features(nanoxgen_xgen_probe PRIVATE cxx_std_17)
  add_executable(nanoxgen_xgen_parity tools/xgen_parity.cpp)
  target_link_libraries(nanoxgen_xgen_parity PRIVATE nanoxgen XGen::XGen)
  target_compile_features(nanoxgen_xgen_parity PRIVATE cxx_std_20)
  add_executable(nanoxgen_xgen_benchmark tools/xgen_benchmark.cpp)
  target_link_libraries(nanoxgen_xgen_benchmark PRIVATE XGen::XGen)
  target_compile_features(nanoxgen_xgen_benchmark PRIVATE cxx_std_17)
  add_executable(nanoxgen_xgen_cache_oracle tools/xgen_cache.cpp)
  target_link_libraries(nanoxgen_xgen_cache_oracle PRIVATE nanoxgen XGen::XGen)
  target_compile_features(nanoxgen_xgen_cache_oracle PRIVATE cxx_std_20)
  add_executable(nanoxgen_xgen_modifier_probe tools/xgen_modifier_probe.cpp)
  target_link_libraries(nanoxgen_xgen_modifier_probe PRIVATE nanoxgen XGen::XGen)
  target_compile_features(nanoxgen_xgen_modifier_probe PRIVATE cxx_std_20)
  set_target_properties(
    nanoxgen_xgen_probe
    nanoxgen_xgen_parity
    nanoxgen_xgen_benchmark
    nanoxgen_xgen_cache_oracle
    nanoxgen_xgen_modifier_probe
    PROPERTIES SKIP_BUILD_RPATH TRUE)
endif()
