# Finds the XGen SDK shipped inside a full Autodesk Maya installation.
#
# Set XGEN_ROOT to a directory such as:
#   /usr/autodesk/maya2027/plug-ins/xgen
#
# Defines:
#   XGen_FOUND
#   XGen::XGen

set(_XGEN_HINTS
  "${XGEN_ROOT}"
  "$ENV{XGEN_ROOT}"
  "${MAYA_LOCATION}/plug-ins/xgen"
  "$ENV{MAYA_LOCATION}/plug-ins/xgen"
  "/usr/autodesk/maya2027/plug-ins/xgen"
  "/usr/autodesk/maya2026/plug-ins/xgen"
  "/usr/autodesk/maya2025/plug-ins/xgen")

find_path(XGen_INCLUDE_DIR
  NAMES
    xgen/src/xgsculptcore/api/XgSplineAPI.h
    XGen/XgSplineAPI.h
  HINTS ${_XGEN_HINTS}
  PATH_SUFFIXES include)

find_library(XGen_LIBRARY
  NAMES AdskXGen libAdskXGen
  HINTS ${_XGEN_HINTS}
  PATH_SUFFIXES lib)

find_library(XGen_CLEW_LIBRARY
  NAMES clew libclew
  HINTS ${_XGEN_HINTS}
  PATH_SUFFIXES lib ../../lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(XGen
  REQUIRED_VARS XGen_INCLUDE_DIR XGen_LIBRARY XGen_CLEW_LIBRARY)

if(XGen_FOUND AND NOT TARGET XGen::XGen)
  add_library(XGen::XGen UNKNOWN IMPORTED)
  set_target_properties(XGen::XGen PROPERTIES
    IMPORTED_LOCATION "${XGen_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${XGen_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${XGen_CLEW_LIBRARY}")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Autodesk's shared objects carry dependencies supplied by Maya's supported
    # distro rather than by the XGen SDK target itself. Match the standalone
    # calibration scripts and defer those transitive resolutions to runtime.
    set_property(TARGET XGen::XGen APPEND PROPERTY
      INTERFACE_LINK_OPTIONS "-Wl,--allow-shlib-undefined")
  endif()
  set(_XGEN_COMPAT_LIB_DIR
    "${NANOXGEN_COMPAT_LIB_DIR}")
  if(NOT _XGEN_COMPAT_LIB_DIR)
    set(_XGEN_COMPAT_LIB_DIR "$ENV{NANOXGEN_COMPAT_LIB_DIR}")
  endif()
  if(_XGEN_COMPAT_LIB_DIR)
    if(NOT IS_DIRECTORY "${_XGEN_COMPAT_LIB_DIR}")
      message(FATAL_ERROR
        "NANOXGEN_COMPAT_LIB_DIR is not a directory: ${_XGEN_COMPAT_LIB_DIR}")
    endif()
    set_property(TARGET XGen::XGen APPEND PROPERTY
      INTERFACE_LINK_DIRECTORIES "${_XGEN_COMPAT_LIB_DIR}")
    set_property(TARGET XGen::XGen APPEND PROPERTY
      INTERFACE_LINK_OPTIONS "-Wl,-rpath,${_XGEN_COMPAT_LIB_DIR}")
  endif()
endif()

mark_as_advanced(XGen_INCLUDE_DIR XGen_LIBRARY XGen_CLEW_LIBRARY)
