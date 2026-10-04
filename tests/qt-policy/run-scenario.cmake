# Configures the Qt licensing policy fixture for one scenario and checks the outcome. Run by CTest (see tests/CMakeLists.txt).
# The scenarios named "allowed..." must configure; every other one must fail, with the expected explanation.

set(expectations
  "forbidden-requested|Qt module 'Charts' is forbidden"
  "forbidden-linked|target 'app' links the forbidden Qt module 'Charts'"
  "forbidden-found|Qt module 'Quick3D' is forbidden \\(GPL-only or commercial-only\\), but it was found or linked"
  "unreviewed-linked|links the Qt module 'Sql', which has not been reviewed"
  "static-qt|Qt must be linked dynamically"
  "static-plugin|may link the Qt plugin 'FakePermissionPlugin' statically"
  "forbidden-plugin|may link the plugin 'FakeSceneParserPlugin' of the forbidden Qt module 'Quick3D'"
  "qmlsc|commercial-only QML compiler qmlsc"
  "allowed|Qt licensing policy: OK"
  "allowed-excluded-plugin|Qt licensing policy: OK")

set(expected "")
foreach(entry IN LISTS expectations)
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 name)
  if(name STREQUAL SCENARIO)
    list(GET parts 1 expected)
  endif()
endforeach()
if(expected STREQUAL "")
  message(FATAL_ERROR "No expectation for scenario '${SCENARIO}'")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
set(generator_args -G "${GENERATOR}")
if(MAKE_PROGRAM)
  list(APPEND generator_args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}" -B "${WORK_DIR}" ${generator_args}
    "-DSCENARIO=${SCENARIO}" "-DPOLICY_MODULE=${POLICY_MODULE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output)

message("${output}")
if(SCENARIO MATCHES "^allowed")
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Configuration failed, but this scenario uses only allowed modules.")
  endif()
elseif(result EQUAL 0)
  message(FATAL_ERROR "Configuration succeeded, but the Qt licensing policy should have rejected scenario '${SCENARIO}'.")
endif()
if(NOT output MATCHES "${expected}")
  message(FATAL_ERROR "The output does not explain the outcome; expected to find: ${expected}")
endif()
message(STATUS "Scenario '${SCENARIO}': outcome and explanation as expected")
