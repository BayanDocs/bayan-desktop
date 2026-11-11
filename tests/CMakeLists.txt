# The tests' strict checker of the engine protocol's JSON Schema, with the copy of the schema from src/engine/stub/schema compiled in as the
# resource :/bayan/test/engine-protocol.schema.json, and the helpers through which every test hands the shell messages as if from the engine,
# which check each message against it first (DESK-003).
add_library(bayan_protocol_schema STATIC support/protocolschema.cpp support/protocolschema.h)
target_include_directories(bayan_protocol_schema PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(bayan_protocol_schema PUBLIC Qt6::Core PRIVATE bayan::compile_options)
qt_add_resources(bayan_protocol_schema protocol_schema
  PREFIX "/bayan/test"
  BASE "${PROJECT_SOURCE_DIR}/src/engine/stub/schema"
  FILES "${PROJECT_SOURCE_DIR}/src/engine/stub/schema/engine-protocol.schema.json")
add_library(bayan_engine_messages STATIC support/enginemessages.cpp support/enginemessages.h)
target_link_libraries(bayan_engine_messages PUBLIC bayan_protocol_schema bayan_engine PRIVATE bayan::compile_options)

# Unit tests for the engine wrapper, run against the stub engine.
qt_add_executable(tst_engine tst_engine.cpp)
target_link_libraries(tst_engine PRIVATE bayan_engine bayan_engine_messages Qt6::Test bayan::compile_options)
qt_import_plugins(tst_engine EXCLUDE_BY_TYPE permissions) # See src/CMakeLists.txt.
bayan_deploy_engine_runtime(tst_engine)
add_test(NAME engine.wrapper COMMAND tst_engine)

# Tests of the engine's C interface itself (bayan_ffi.h), with a callback of their own, against the engine the build links: what the stub must
# do as the real engine does (BAYAN_STATUS_WRONG_THREAD inside callbacks, tiles in order with messages, freeing inside the callback), and
# every kind of message the engine sends, checked against the protocol's JSON Schema.
qt_add_executable(tst_cinterface tst_cinterface.cpp)
target_link_libraries(tst_cinterface PRIVATE bayan_engine_c bayan_protocol_schema Qt6::Test bayan::compile_options)
qt_import_plugins(tst_cinterface EXCLUDE_BY_TYPE permissions) # See src/CMakeLists.txt.
bayan_deploy_engine_runtime(tst_cinterface)
add_test(NAME engine.c_interface COMMAND tst_cinterface)

# With an SDK engine: the copies of the SDK's C header and JSON Schema in src/engine/stub must be byte for byte the SDK's, so that moving the
# bayan-core pin without updating them fails (src/engine/README.md).
if(BAYAN_ENGINE STREQUAL "sdk")
  add_test(NAME engine.sdk_copies
    COMMAND "${CMAKE_COMMAND}"
      -D "SDK_DIR=${BAYAN_ENGINE_SDK_DIR}"
      -D "COPY_DIR=${PROJECT_SOURCE_DIR}/src/engine/stub"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/engine-sdk/compare-copies.cmake")
  set_tests_properties(engine.sdk_copies PROPERTIES TIMEOUT 60)
endif()

# Unit tests for the document session (src/app).
qt_add_executable(tst_documentsession tst_documentsession.cpp)
target_link_libraries(tst_documentsession PRIVATE bayan_shell bayan_engine_messages Qt6::Test bayan::compile_options)
qt_import_plugins(tst_documentsession EXCLUDE_BY_TYPE permissions) # See src/CMakeLists.txt.
bayan_deploy_engine_runtime(tst_documentsession)
add_test(NAME app.document_session COMMAND tst_documentsession)

# Tests of the document canvas in a window (src/canvas, src/a11y): pixels, scrolling, zoom, keys, input-method composition and the
# accessibility bridge, against the engine the build links. Tests that need the real engine's mock document skip with the stub.
qt_add_executable(tst_canvas tst_canvas.cpp)
target_link_libraries(tst_canvas PRIVATE bayan_shell Qt6::Quick Qt6::Test bayan::compile_options)
qt_import_plugins(tst_canvas EXCLUDE_BY_TYPE permissions) # See src/CMakeLists.txt.
bayan_deploy_engine_runtime(tst_canvas)
add_test(NAME app.canvas COMMAND tst_canvas)
# The same at 125 % scaling, a fractional device pixel ratio as on many Windows laptops: tiles must still land on whole device pixels.
add_test(NAME app.canvas_scale125 COMMAND tst_canvas)

# Tests of the interface in QML (src/qml) around the canvas, against the engine the build links: the "Pages" item and its keys, the zoom
# buttons and their announcements, plain text for engine strings, and what the interface does after engine failures (engine protocol §12.1).
qt_add_executable(tst_interface tst_interface.cpp)
# bayan_shellplugin is the QML module's static plugin; tst_interface.cpp imports it with Q_IMPORT_QML_PLUGIN, as main.cpp does.
target_link_libraries(tst_interface PRIVATE bayan_shell bayan_shellplugin Qt6::Quick Qt6::QuickControls2 Qt6::Test bayan::compile_options)
qt_import_plugins(tst_interface EXCLUDE_BY_TYPE permissions) # See src/CMakeLists.txt.
bayan_deploy_engine_runtime(tst_interface)
add_test(NAME app.interface COMMAND tst_interface)

# Unit tests of the shell's pure logic, which needs no engine: the protocol parsers, the tile arithmetic and cache, the key and input-method
# translation, the accessible text, and the allowance of restarts after engine failures.
foreach(unit IN ITEMS protocol tiles input accessibletext restartallowance)
  qt_add_executable(tst_${unit} tst_${unit}.cpp)
  target_link_libraries(tst_${unit} PRIVATE bayan_shell Qt6::Test bayan::compile_options)
  qt_import_plugins(tst_${unit} EXCLUDE_BY_TYPE permissions) # See src/CMakeLists.txt.
  bayan_deploy_engine_runtime(tst_${unit})
  add_test(NAME unit.${unit} COMMAND tst_${unit})
  set_tests_properties(unit.${unit} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1" TIMEOUT 60)
endforeach()

# Unit tests of the tests' checker of the engine protocol's JSON Schema (support/protocolschema.h), which needs no engine.
qt_add_executable(tst_protocolschema tst_protocolschema.cpp)
target_link_libraries(tst_protocolschema PRIVATE bayan_protocol_schema Qt6::Test bayan::compile_options)
add_test(NAME unit.protocol_schema COMMAND tst_protocolschema)
set_tests_properties(unit.protocol_schema PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1" TIMEOUT 60)

# Headless smoke test: the application starts with the offscreen platform plugin, shows the engine's first page, checks its pixels and exits.
add_test(NAME app.smoke COMMAND bayan-desktop --smoke-test)

# Every test runs without a screen and with Qt's standard settings, whatever the developer's environment says.
set_tests_properties(engine.wrapper engine.c_interface app.document_session app.canvas app.interface app.smoke PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1"
  TIMEOUT 120)
set_tests_properties(app.canvas_scale125 PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.25"
  TIMEOUT 120)

# The Qt licensing policy (cmake/BayanQtPolicy.cmake): configuring small fixture projects must fail for forbidden, unreviewed or static Qt
# modules and for qmlsc, and succeed for a reviewed shared module. The fixtures define stand-in Qt targets, so no GPL module is ever needed.
foreach(scenario IN ITEMS forbidden-requested forbidden-linked forbidden-found unreviewed-linked static-qt static-plugin forbidden-plugin qmlsc
                         allowed allowed-excluded-plugin)
  add_test(NAME qt_policy.${scenario}
    COMMAND "${CMAKE_COMMAND}"
      -D "SCENARIO=${scenario}"
      -D "FIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/qt-policy"
      -D "WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/qt-policy/${scenario}"
      -D "POLICY_MODULE=${PROJECT_SOURCE_DIR}/cmake/BayanQtPolicy.cmake"
      -D "GENERATOR=${CMAKE_GENERATOR}"
      -D "MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/qt-policy/run-scenario.cmake")
endforeach()

# The Qt installer (scripts/install-qt.py), run against a small fake Qt repository on disk.
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_test(NAME install_qt COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/install-qt/test_install_qt.py")
set_tests_properties(install_qt PROPERTIES
  ENVIRONMENT "BAYAN_INSTALL_QT=${PROJECT_SOURCE_DIR}/scripts/install-qt.py;BAYAN_TEST_CMAKE=${CMAKE_COMMAND}"
  TIMEOUT 120)

# The Qt release-age check (scripts/check-qt-release-age.py, ADR-0017 rule 4): its tests, against small Git repositories made on the fly, and
# the check itself on this repository: the Qt release pinned in deps/qt.json must be at least 24 hours older than the commit that pinned it.
# The check reads the Git history, so a shallow clone fails it (CI checks out with fetch-depth: 0); a source tree without Git skips it.
add_test(NAME supply_chain.qt_release_age_tests COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/qt-release-age/test_check_qt_release_age.py")
set_tests_properties(supply_chain.qt_release_age_tests PROPERTIES
  ENVIRONMENT "BAYAN_CHECK_QT_RELEASE_AGE=${PROJECT_SOURCE_DIR}/scripts/check-qt-release-age.py"
  TIMEOUT 120)
add_test(NAME supply_chain.qt_release_age COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/scripts/check-qt-release-age.py" --root "${PROJECT_SOURCE_DIR}")
set_tests_properties(supply_chain.qt_release_age PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# The Ubuntu snapshot and package list are set in two workflows, ci.yml and the C++ job of codeql.yml (X-002); this keeps them equal.
add_test(NAME supply_chain.workflow_ubuntu_packages COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/workflows/test_ubuntu_packages.py")
set_tests_properties(supply_chain.workflow_ubuntu_packages PROPERTIES
  ENVIRONMENT "BAYAN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
  TIMEOUT 60)
