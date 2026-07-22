# Finds the public Maya C++ API shipped inside a complete Maya installation.
#
# Set MAYA_LOCATION to a directory such as /usr/autodesk/maya2027.
#
# Defines:
#   Maya_FOUND
#   Maya::Foundation
#   Maya::OpenMaya

set(_MAYA_HINTS
  "${MAYA_LOCATION}"
  "$ENV{MAYA_LOCATION}"
  "/usr/autodesk/maya2027"
  "/usr/autodesk/maya2026"
  "/usr/autodesk/maya2025")

find_path(Maya_INCLUDE_DIR
  NAMES maya/MFnPlugin.h
  HINTS ${_MAYA_HINTS}
  PATH_SUFFIXES include)
find_library(Maya_FOUNDATION_LIBRARY
  NAMES Foundation libFoundation
  HINTS ${_MAYA_HINTS}
  PATH_SUFFIXES lib)
find_library(Maya_OPENMAYA_LIBRARY
  NAMES OpenMaya libOpenMaya
  HINTS ${_MAYA_HINTS}
  PATH_SUFFIXES lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Maya
  REQUIRED_VARS Maya_INCLUDE_DIR Maya_FOUNDATION_LIBRARY Maya_OPENMAYA_LIBRARY)

if(Maya_FOUND AND NOT TARGET Maya::Foundation)
  add_library(Maya::Foundation UNKNOWN IMPORTED)
  set_target_properties(Maya::Foundation PROPERTIES
    IMPORTED_LOCATION "${Maya_FOUNDATION_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${Maya_INCLUDE_DIR}")
endif()

if(Maya_FOUND AND NOT TARGET Maya::OpenMaya)
  add_library(Maya::OpenMaya UNKNOWN IMPORTED)
  set_target_properties(Maya::OpenMaya PROPERTIES
    IMPORTED_LOCATION "${Maya_OPENMAYA_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${Maya_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "Maya::Foundation")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_property(TARGET Maya::OpenMaya APPEND PROPERTY
      INTERFACE_LINK_OPTIONS "-Wl,--allow-shlib-undefined")
  endif()
endif()

mark_as_advanced(
  Maya_INCLUDE_DIR Maya_FOUNDATION_LIBRARY Maya_OPENMAYA_LIBRARY)
