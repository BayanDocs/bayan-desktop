# Fixture for the Qt licensing policy tests (see run-scenario.cmake). It needs no compiler and no Qt: it creates stand-in imported targets
# named like Qt's and lets cmake/BayanQtPolicy.cmake judge them.
cmake_minimum_required(VERSION 3.25...4.4)
project(qt_policy_fixture LANGUAGES NONE)

include("${POLICY_MODULE}")

# A stand-in for a Qt module: an imported library with Qt's target name.
function(fake_qt_module name type)
  add_library(Qt6::${name} ${type} IMPORTED)
endfunction()

add_library(app INTERFACE)

if(SCENARIO STREQUAL "forbidden-requested")
  # Asking for a forbidden module fails before CMake even looks for it.
  bayan_find_qt(COMPONENTS Core Charts)
elseif(SCENARIO STREQUAL "forbidden-linked")
  fake_qt_module(Core SHARED)
  fake_qt_module(Charts SHARED)
  target_link_libraries(app INTERFACE Qt6::Core Qt6::Charts)
elseif(SCENARIO STREQUAL "forbidden-found")
  # Found (for example as a dependency of another module) but not linked by us: still refused.
  fake_qt_module(Quick3D SHARED)
elseif(SCENARIO STREQUAL "unreviewed-linked")
  fake_qt_module(Sql SHARED)
  target_link_libraries(app INTERFACE Qt6::Sql)
elseif(SCENARIO STREQUAL "static-qt")
  fake_qt_module(Core STATIC)
  target_link_libraries(app INTERFACE Qt6::Core)
elseif(SCENARIO STREQUAL "qmlsc")
  add_executable(Qt6::qmlsc IMPORTED)
elseif(SCENARIO STREQUAL "allowed")
  fake_qt_module(Core SHARED)
  fake_qt_module(Quick SHARED)
  target_link_libraries(app INTERFACE Qt6::Core $<LINK_ONLY:Qt6::Quick>)
else()
  message(FATAL_ERROR "unknown scenario '${SCENARIO}'")
endif()
