# Locate a separately built LuisaCompute `next` checkout without vendoring it
# into NanoXGen. The upstream HIP backend currently assumes it is configured as
# the top-level project, so consuming its build tree is more reliable than
# add_subdirectory and keeps all third-party artifacts outside this repository.

set(LUISA_COMPUTE_SOURCE_DIR "" CACHE PATH
    "LuisaCompute next source checkout (cloned recursively)")
set(LUISA_COMPUTE_BUILD_DIR "" CACHE PATH
    "Matching standalone LuisaCompute build directory")

find_path(LuisaComputeNext_INCLUDE_DIR
    NAMES luisa/runtime/context.h
    PATHS "${LUISA_COMPUTE_SOURCE_DIR}/include"
    NO_DEFAULT_PATH)
find_path(LuisaComputeNext_GENERATED_INCLUDE_DIR
    NAMES glslang/build_info.h
    PATHS "${LUISA_COMPUTE_BUILD_DIR}/include"
    NO_DEFAULT_PATH)

set(_luisa_next_library_names core ast ir runtime dsl xir)
set(_luisa_next_libraries)
foreach(_component IN LISTS _luisa_next_library_names)
    string(TOUPPER "${_component}" _component_upper)
    find_library(LuisaComputeNext_${_component_upper}_LIBRARY
        NAMES "luisa-${_component}"
        PATHS "${LUISA_COMPUTE_BUILD_DIR}/bin"
        NO_DEFAULT_PATH)
    list(APPEND _luisa_next_libraries
        "${LuisaComputeNext_${_component_upper}_LIBRARY}")
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LuisaComputeNext
    REQUIRED_VARS
        LUISA_COMPUTE_SOURCE_DIR
        LUISA_COMPUTE_BUILD_DIR
        LuisaComputeNext_INCLUDE_DIR
        LuisaComputeNext_GENERATED_INCLUDE_DIR
        LuisaComputeNext_CORE_LIBRARY
        LuisaComputeNext_AST_LIBRARY
        LuisaComputeNext_IR_LIBRARY
        LuisaComputeNext_RUNTIME_LIBRARY
        LuisaComputeNext_DSL_LIBRARY
        LuisaComputeNext_XIR_LIBRARY)

if(LuisaComputeNext_FOUND AND NOT TARGET LuisaComputeNext::DSL)
    set(_luisa_next_include_dirs
        "${LuisaComputeNext_INCLUDE_DIR}"
        "${LuisaComputeNext_GENERATED_INCLUDE_DIR}"
        "${LUISA_COMPUTE_SOURCE_DIR}/src/ext/spdlog/include"
        "${LUISA_COMPUTE_SOURCE_DIR}/src/ext/xxHash"
        "${LUISA_COMPUTE_SOURCE_DIR}/src/ext/magic_enum/include"
        "${LUISA_COMPUTE_SOURCE_DIR}/src/ext/EASTL/include"
        "${LUISA_COMPUTE_SOURCE_DIR}/src/ext/EASTL/packages/EABase/include/Common"
        "${LUISA_COMPUTE_SOURCE_DIR}/src/ext/reproc/reproc/include"
        "${LUISA_COMPUTE_SOURCE_DIR}/src/ext/reproc/reproc++/include"
        "${LUISA_COMPUTE_SOURCE_DIR}/src/ext/marl/include"
        "${LUISA_COMPUTE_SOURCE_DIR}/src/ext/half/include"
        "${LUISA_COMPUTE_SOURCE_DIR}/src/ext/stb"
        "${LUISA_COMPUTE_SOURCE_DIR}/src/ext/glslang"
        "${LUISA_COMPUTE_SOURCE_DIR}/src/ext/yyjson/src")

    add_library(LuisaComputeNext::DSL INTERFACE IMPORTED)
    set_target_properties(LuisaComputeNext::DSL PROPERTIES
        INTERFACE_COMPILE_FEATURES cxx_std_20
        INTERFACE_INCLUDE_DIRECTORIES "${_luisa_next_include_dirs}"
        INTERFACE_COMPILE_DEFINITIONS
            "EASTL_ALLOCATOR_EXPLICIT_ENABLED=1;EASTL_DEPRECATIONS_FOR_2024_APRIL=EA_DISABLED;EASTL_HAVE_CPP11_TYPE_TRAITS=1;EASTL_INLINE_NAMESPACES_ENABLED=1;EASTL_INLINE_VARIABLE_ENABLED=1;EASTL_MOVE_SEMANTICS_ENABLED=1;EASTL_STD_ITERATOR_CATEGORY_ENABLED=1;EASTL_STD_TYPE_TRAITS_AVAILABLE=1;EASTL_USER_DEFINED_ALLOCATOR=1;EASTL_USER_LITERALS_ENABLED=0;EASTL_VARIABLE_TEMPLATES_ENABLED=1;EASTL_VARIADIC_TEMPLATES_ENABLED=1;EA_DLL=1;EA_HAVE_CPP11_ATOMIC=1;EA_HAVE_CPP11_CHRONO=1;EA_HAVE_CPP11_CONDITION_VARIABLE=1;EA_HAVE_CPP11_CONTAINERS=1;EA_HAVE_CPP11_FUTURE=1;EA_HAVE_CPP11_INITIALIZER_LIST=1;EA_HAVE_CPP11_MUTEX=1;EA_HAVE_CPP11_RANDOM=1;EA_HAVE_CPP11_REGEX=1;EA_HAVE_CPP11_SCOPED_ALLOCATOR=1;EA_HAVE_CPP11_SYSTEM_ERROR=1;EA_HAVE_CPP11_THREAD=1;EA_HAVE_CPP11_TUPLES=1;EA_HAVE_CPP11_TYPEINDEX=1;EA_HAVE_CPP11_TYPE_TRAITS=1;EA_PRAGMA_ONCE_SUPPORTED=1;FMT_EXCEPTIONS=0;FMT_HEADER_ONLY=1;FMT_USE_CONSTEVAL=0;FMT_USE_NOEXCEPT=1;LUISA_ENABLE_DSL=1;LUISA_ENABLE_IR=1;LUISA_ENABLE_XIR=1;LUISA_PLATFORM_UNIX=1;MARL_DLL=1;REPROCXX_SHARED;REPROC_SHARED;SPDLOG_DISABLE_DEFAULT_LOGGER;SPDLOG_NO_EXCEPTIONS;SPDLOG_NO_THREAD_ID;XXH_INLINE_ALL;YYJSON_IMPORTS=1"
        INTERFACE_LINK_LIBRARIES "${_luisa_next_libraries};${CMAKE_DL_LIBS}")
    set(LuisaComputeNext_RUNTIME_DIR "${LUISA_COMPUTE_BUILD_DIR}/bin")
endif()

mark_as_advanced(
    LuisaComputeNext_INCLUDE_DIR
    LuisaComputeNext_GENERATED_INCLUDE_DIR
    LuisaComputeNext_CORE_LIBRARY
    LuisaComputeNext_AST_LIBRARY
    LuisaComputeNext_IR_LIBRARY
    LuisaComputeNext_RUNTIME_LIBRARY
    LuisaComputeNext_DSL_LIBRARY
    LuisaComputeNext_XIR_LIBRARY)
