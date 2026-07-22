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
option(NANOXGEN_BUILD_AUTODESK_BRIDGES
  "Build explicit Autodesk XGen/Maya renderer bridges" OFF)
option(NANOXGEN_ENABLE_CUDA "Build the CUDA generation kernel" OFF)
option(NANOXGEN_ENABLE_HIP "Build the HIP generation and cache-residency kernels" OFF)
option(NANOXGEN_ENABLE_LUISA
  "Build the optional LuisaCompute next JIT execution bridge" OFF)
option(NANOXGEN_ENABLE_ALEMBIC
  "Build the optional Classic XGen Alembic patch importer" OFF)
set(NANOXGEN_LUISA_BACKEND "hip" CACHE STRING
  "LuisaCompute runtime backend used by the optional parity test")
set_property(CACHE NANOXGEN_LUISA_BACKEND PROPERTY STRINGS hip vk cpu fallback)
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
  src/xgen_classic.cpp
  src/xgen_expression.cpp
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

add_executable(nanoxgen_xgen_classic_inspect tools/xgen_classic_inspect.cpp)
target_link_libraries(nanoxgen_xgen_classic_inspect PRIVATE nanoxgen)

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
  nanoxgen_xgen_classic_inspect
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
  # test_main exercises the public host/device inline generation functions
  # directly and compares them bit-for-bit with the library entry points. Keep
  # both translation units on the same native/SIMD/fast-math contract.
  target_link_libraries(nanoxgen_tests PRIVATE nanoxgen nanoxgen_build_options)
  add_test(NAME nanoxgen_tests COMMAND nanoxgen_tests)
  add_executable(nanoxgen_package_tests tests/package_test.cpp)
  target_link_libraries(nanoxgen_package_tests PRIVATE nanoxgen)
  add_test(NAME nanoxgen_package_tests COMMAND nanoxgen_package_tests)
  add_executable(nanoxgen_classic_tests tests/classic_test.cpp)
  target_link_libraries(nanoxgen_classic_tests PRIVATE nanoxgen)
  add_test(NAME nanoxgen_classic_tests COMMAND nanoxgen_classic_tests)
  add_executable(nanoxgen_expression_tests tests/expression_test.cpp)
  target_link_libraries(nanoxgen_expression_tests PRIVATE nanoxgen)
  add_test(NAME nanoxgen_expression_tests COMMAND nanoxgen_expression_tests)
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
    target_link_libraries(
      nanoxgen_cuda_tests PRIVATE nanoxgen_cuda nanoxgen_build_options)
    add_test(NAME nanoxgen_cuda_tests COMMAND nanoxgen_cuda_tests)
    set_tests_properties(nanoxgen_cuda_tests PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

if(NANOXGEN_ENABLE_HIP)
  enable_language(HIP)
  find_package(hip REQUIRED CONFIG)
  add_library(nanoxgen_hip STATIC src/hip/generate.hip)
  set_source_files_properties(src/hip/generate.hip PROPERTIES LANGUAGE HIP)
  target_include_directories(nanoxgen_hip PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(nanoxgen_hip PUBLIC hip_std_20)
  target_link_libraries(nanoxgen_hip PUBLIC nanoxgen hip::host)
  if(NANOXGEN_FAST_MATH)
    target_compile_options(nanoxgen_hip PRIVATE
      $<$<COMPILE_LANGUAGE:HIP>:-ffast-math>)
  endif()

  add_executable(nanoxgen_hip_benchmark tools/hip_benchmark.cpp)
  target_link_libraries(nanoxgen_hip_benchmark PRIVATE nanoxgen_hip)

  add_executable(nanoxgen_hip_cache_benchmark tools/hip_cache_benchmark.cpp)
  set_source_files_properties(tools/hip_cache_benchmark.cpp PROPERTIES LANGUAGE HIP)
  target_link_libraries(nanoxgen_hip_cache_benchmark PRIVATE nanoxgen hip::host)
  target_compile_features(nanoxgen_hip_cache_benchmark PRIVATE hip_std_20)

  if(NANOXGEN_BUILD_TESTS)
    add_executable(nanoxgen_hip_tests tests/hip_test.cpp)
    target_link_libraries(nanoxgen_hip_tests PRIVATE nanoxgen_hip nanoxgen_build_options)
    add_test(NAME nanoxgen_hip_tests COMMAND nanoxgen_hip_tests)
    set_tests_properties(nanoxgen_hip_tests PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

if(NANOXGEN_ENABLE_LUISA)
  list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
  find_package(LuisaComputeNext REQUIRED)
  set(_nanoxgen_luisa_plugin
    "${LuisaComputeNext_RUNTIME_DIR}/${CMAKE_SHARED_MODULE_PREFIX}luisa-backend-${NANOXGEN_LUISA_BACKEND}${CMAKE_SHARED_MODULE_SUFFIX}")
  if(NOT EXISTS "${_nanoxgen_luisa_plugin}")
    message(FATAL_ERROR
      "LuisaCompute backend plug-in not found: ${_nanoxgen_luisa_plugin}")
  endif()
  add_library(nanoxgen_luisa STATIC src/luisa/xgen_expression.cpp)
  add_library(NanoXGen::nanoxgen_luisa ALIAS nanoxgen_luisa)
  target_include_directories(
    nanoxgen_luisa PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(nanoxgen_luisa PUBLIC cxx_std_20)
  target_link_libraries(
    nanoxgen_luisa PUBLIC nanoxgen LuisaComputeNext::DSL)
  add_executable(nanoxgen_luisa_tests tests/luisa_test.cpp)
  target_link_libraries(
    nanoxgen_luisa_tests PRIVATE nanoxgen_luisa)
  if(NANOXGEN_BUILD_TESTS)
    add_test(NAME nanoxgen_luisa_tests
      COMMAND nanoxgen_luisa_tests
        "${LuisaComputeNext_RUNTIME_DIR}" "${NANOXGEN_LUISA_BACKEND}")
  endif()
endif()

if(NANOXGEN_ENABLE_ALEMBIC)
  find_package(Alembic CONFIG REQUIRED)
  find_package(TBB CONFIG REQUIRED)
  find_package(OpenSubdiv CONFIG REQUIRED)
  add_library(nanoxgen_classic_alembic STATIC src/xgen_classic_alembic.cpp)
  add_library(NanoXGen::nanoxgen_classic_alembic ALIAS nanoxgen_classic_alembic)
  target_include_directories(
    nanoxgen_classic_alembic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(nanoxgen_classic_alembic PUBLIC cxx_std_20)
  target_link_libraries(
    nanoxgen_classic_alembic
    PUBLIC nanoxgen Alembic::Alembic OpenSubdiv::osdCPU)
  target_link_libraries(
    nanoxgen_classic_alembic PRIVATE nanoxgen_build_options)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
      nanoxgen_classic_alembic PRIVATE -Wall -Wextra -Wpedantic)
  elseif(MSVC)
    target_compile_options(nanoxgen_classic_alembic PRIVATE /W4)
  endif()
  add_executable(
    nanoxgen_xgen_classic_asset tools/xgen_classic_asset.cpp)
  target_link_libraries(
    nanoxgen_xgen_classic_asset PRIVATE nanoxgen_classic_alembic)
  if(NANOXGEN_BUILD_TESTS)
    add_executable(
      nanoxgen_classic_alembic_tests tests/classic_alembic_test.cpp)
    target_link_libraries(
      nanoxgen_classic_alembic_tests PRIVATE nanoxgen_classic_alembic)
    add_test(
      NAME nanoxgen_classic_alembic_tests
      COMMAND nanoxgen_classic_alembic_tests)
  endif()
endif()

if(NANOXGEN_BUILD_CALIBRATION OR NANOXGEN_BUILD_AUTODESK_BRIDGES)
  list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
  find_package(XGen REQUIRED)
endif()

if(NANOXGEN_BUILD_AUTODESK_BRIDGES)
  find_package(Maya REQUIRED)
endif()

if(NANOXGEN_BUILD_CALIBRATION)
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
  add_executable(nanoxgen_xgen_expression_oracle tools/xgen_expression_oracle.cpp)
  target_link_libraries(nanoxgen_xgen_expression_oracle PRIVATE XGen::XGen)
  target_compile_features(nanoxgen_xgen_expression_oracle PRIVATE cxx_std_20)
  set_target_properties(
    nanoxgen_xgen_probe
    nanoxgen_xgen_parity
    nanoxgen_xgen_benchmark
    nanoxgen_xgen_cache_oracle
    nanoxgen_xgen_modifier_probe
    nanoxgen_xgen_expression_oracle
    PROPERTIES SKIP_BUILD_RPATH TRUE)
endif()

if(NANOXGEN_BUILD_AUTODESK_BRIDGES)
  set_property(TARGET nanoxgen PROPERTY POSITION_INDEPENDENT_CODE ON)
  add_executable(nanoxgen_xgen_classic_typed tools/xgen_classic_typed.cpp)
  target_link_libraries(nanoxgen_xgen_classic_typed PRIVATE nanoxgen XGen::XGen)
  target_compile_features(nanoxgen_xgen_classic_typed PRIVATE cxx_std_20)
  add_library(nanoxgen_maya_xgen_cache MODULE tools/maya_xgen_cache.cpp)
  target_link_libraries(
    nanoxgen_maya_xgen_cache PRIVATE nanoxgen Maya::OpenMaya)
  target_compile_features(nanoxgen_maya_xgen_cache PRIVATE cxx_std_20)
  set_target_properties(
    nanoxgen_xgen_classic_typed
    nanoxgen_maya_xgen_cache
    PROPERTIES SKIP_BUILD_RPATH TRUE)
  set_target_properties(
    nanoxgen_maya_xgen_cache PROPERTIES PREFIX "" SUFFIX ".so")
endif()
