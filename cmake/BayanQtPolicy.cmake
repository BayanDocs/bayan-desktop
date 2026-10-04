# Qt licensing policy (ADR-0013, ADR-0003): bayan-desktop may use only LGPLv3 Qt modules, linked dynamically, and never commercial-only tools such as qmlsc.
#
# Two lists enforce it:
# - BAYAN_QT_FORBIDDEN_MODULES: modules that are GPL-only or commercial-only. Configuration fails if any of them is found or linked anywhere in the project, even indirectly.
# - BAYAN_QT_ALLOWED_MODULES: the modules whose LGPL licensing has been reviewed. Our own targets may link only these; anything else fails configuration until someone checks its license and adds it here in a reviewed pull request.
#
# Licenses were checked against the Qt 6.12 module documentation on doc.qt.io on 2026-10-04 (the "Licenses" section of each module's index page).
# Use bayan_find_qt() instead of find_package(Qt6 ...); the checks run automatically at the end of configuration.

include_guard(GLOBAL)

# Regular expressions over module names, as they appear in the imported targets Qt6::<Module> and Qt::<Module> (including <Module>Private and related helper libraries).
set(BAYAN_QT_FORBIDDEN_MODULES
  # Named in ADR-0013.
  "^Charts" "^Graphs" "^Grpc" "^Quick3D" "^VirtualKeyboard" "^HunspellInputMethod" "^CanvasPainter"
  # Also GPL-only according to the Qt 6.12 documentation.
  "^DataVisualization" "^NetworkAuth" "^QuickTimeline" "^Lottie" "^Bodymovin" "^HttpServer" "^Coap" "^Mqtt"
  "^WaylandCompositor"
)

# Exact module names (without the Qt6:: prefix) that our targets may link directly. All are LGPLv3 (or GPLv2) per the Qt 6.12 documentation.
set(BAYAN_QT_ALLOWED_MODULES
  Core Gui Qml QmlIntegration QmlMeta QmlModels QmlWorkerScript Quick QuickControls2 Test QuickTest
)

set(BAYAN_QT_POLICY_DOC "ADR-0013 and cmake/BayanQtPolicy.cmake")

# Sets <out_var> to TRUE if a module name matches one of the forbidden patterns.
function(_bayan_qt_is_forbidden module out_var)
  set(${out_var} FALSE PARENT_SCOPE)
  foreach(pattern IN LISTS BAYAN_QT_FORBIDDEN_MODULES)
    if(module MATCHES "${pattern}")
      set(${out_var} TRUE PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()

# Fails if a requested module is forbidden or has not been reviewed.
function(_bayan_qt_check_components)
  foreach(component IN LISTS ARGN)
    _bayan_qt_is_forbidden("${component}" forbidden)
    if(forbidden)
      message(FATAL_ERROR "Qt module '${component}' is forbidden: it is GPL-only or commercial-only, and bayan-desktop may use only LGPL Qt modules (${BAYAN_QT_POLICY_DOC}).")
    endif()
    if(NOT component IN_LIST BAYAN_QT_ALLOWED_MODULES)
      message(FATAL_ERROR "Qt module '${component}' has not been reviewed for use in bayan-desktop. Check on doc.qt.io that it is available under the LGPL, then add it to BAYAN_QT_ALLOWED_MODULES in a reviewed pull request (${BAYAN_QT_POLICY_DOC}).")
    endif()
  endforeach()
endfunction()

# Replacement for find_package(Qt6 REQUIRED COMPONENTS ...) that refuses forbidden or unreviewed modules before looking for them.
# A macro rather than a function, so that the variables find_package sets stay visible to the caller (Qt's own commands need them).
# Usage: bayan_find_qt(COMPONENTS Core Gui Quick)
macro(bayan_find_qt)
  cmake_parse_arguments(_bayan_find_qt "" "" "COMPONENTS" ${ARGN})
  _bayan_qt_check_components(${_bayan_find_qt_COMPONENTS})
  find_package(Qt6 REQUIRED COMPONENTS ${_bayan_find_qt_COMPONENTS})
endmacro()

# Collects every directory of the project, starting at <dir>.
function(_bayan_collect_directories dir out_var)
  set(result "${dir}")
  get_property(children DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    _bayan_collect_directories("${child}" grandchildren)
    list(APPEND result ${grandchildren})
  endforeach()
  set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# Extracts the Qt module names (Qt6::Name or Qt::Name) mentioned in a list of link items, including inside generator expressions.
function(_bayan_qt_modules_in items out_var)
  set(result "")
  string(REGEX MATCHALL "Qt6?::[A-Za-z0-9_]+" references "${items}")
  foreach(reference IN LISTS references)
    string(REGEX REPLACE "^Qt6?::" "" module "${reference}")
    list(APPEND result "${module}")
  endforeach()
  list(REMOVE_DUPLICATES result)
  set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# Runs at the end of configuration (scheduled below). Checks every directory and target of the project.
function(bayan_qt_policy_check)
  _bayan_collect_directories("${CMAKE_SOURCE_DIR}" directories)
  set(violations "")

  foreach(dir IN LISTS directories)
    # 1. No forbidden module may be found at all: every module that find_package loads, directly or as a dependency, becomes an imported target.
    get_property(imported DIRECTORY "${dir}" PROPERTY IMPORTED_TARGETS)
    _bayan_qt_modules_in("${imported}" imported_modules)
    foreach(module IN LISTS imported_modules)
      _bayan_qt_is_forbidden("${module}" forbidden)
      if(forbidden)
        list(APPEND violations "Qt module '${module}' is forbidden (GPL-only or commercial-only), but it was found or linked")
      endif()
    endforeach()

    # 2. Our own targets may link only reviewed modules, and only as shared libraries (or header-only interface libraries).
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
      get_target_property(link_items "${target}" LINK_LIBRARIES)
      get_target_property(interface_items "${target}" INTERFACE_LINK_LIBRARIES)
      set(items "")
      foreach(list_var IN ITEMS link_items interface_items)
        if(${list_var})
          list(APPEND items ${${list_var}})
        endif()
      endforeach()
      _bayan_qt_modules_in("${items}" modules)
      foreach(module IN LISTS modules)
        _bayan_qt_is_forbidden("${module}" forbidden)
        if(forbidden)
          list(APPEND violations "target '${target}' links the forbidden Qt module '${module}' (GPL-only or commercial-only)")
          continue()
        endif()
        string(REGEX REPLACE "Private$" "" public_module "${module}")
        if(NOT public_module IN_LIST BAYAN_QT_ALLOWED_MODULES)
          list(APPEND violations "target '${target}' links the Qt module '${module}', which has not been reviewed: check on doc.qt.io that it is available under the LGPL, then add it to BAYAN_QT_ALLOWED_MODULES")
          continue()
        endif()
        if(TARGET "Qt6::${module}")
          get_target_property(type "Qt6::${module}" TYPE)
          if(NOT type STREQUAL "SHARED_LIBRARY" AND NOT type STREQUAL "INTERFACE_LIBRARY")
            list(APPEND violations "target '${target}' links Qt6::${module} as a ${type}; Qt must be linked dynamically (LGPL)")
          endif()
        endif()
      endforeach()
    endforeach()
  endforeach()

  # 3. The commercial-only QML compiler must not be used; qmlcachegen compiles QML instead.
  if(TARGET Qt6::qmlsc)
    list(APPEND violations "the commercial-only QML compiler qmlsc is present; use a Qt installation without it, so that qmlcachegen compiles QML")
  endif()

  if(violations)
    list(REMOVE_DUPLICATES violations)
    list(JOIN violations "\n  - " message_text)
    message(FATAL_ERROR "Qt licensing policy violated (${BAYAN_QT_POLICY_DOC}):\n  - ${message_text}")
  endif()
  message(STATUS "Qt licensing policy: OK (only reviewed LGPL modules, linked dynamically)")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL bayan_qt_policy_check)
