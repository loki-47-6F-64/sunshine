# - Try to find Glib
# Once done this will define
#
#  GLIB_FOUND - system has Glib
#  GLIB_INCLUDE_DIRS - the Glib include directory
#  GLIB_LIBRARIES - the libraries needed to use Glib
#  GLIB_DEFINITIONS - Compiler switches required for using Glib

# Use pkg-config to get the directories and then use these values
# in the find_path() and find_library() calls
find_package(PkgConfig)
pkg_check_modules(PC_GLIB gobject-2.0 glib-2.0)

set(GLIB_DEFINITIONS ${PC_GLIB_CFLAGS} ${PC_GOBJECT_CFLAGS})

find_path(GLIB_INCLUDE_DIRS glibconfig.h PATHS ${PC_GLIB_INCLUDEDIR} ${PC_GLIB_INCLUDE_DIRS} PATH_SUFFIXES glib-2.0/include/)
find_library(GLIB_LIBRARIES NAMES glib-2.0 libglib-2.0 glib-unix-2.0 PATHS ${PC_GLIB_LIBDIR} ${PC_GLIB_LIBRARY_DIRS})
find_library(GOBJECT_LIBRARIES NAMES gobject-unix-2.0 gobject-2.0 libgobject-2.0 PATHS ${PC_GOBJECT_LIBDIR} ${PC_GOBJECT_LIBRARY_DIRS})

list(APPEND GLIB_LIBRARIES ${GOBJECT_LIBRARIES})
mark_as_advanced(GLIB_INCLUDE_DIRS GLIB_LIBRARIES)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GLIB REQUIRED_VARS GLIB_LIBRARIES GLIB_INCLUDE_DIRS)

if(GLIB_FOUND)
  set(HAVE_DBUS "1")
else()
  set(HAVE_DBUS "0")
endif()
