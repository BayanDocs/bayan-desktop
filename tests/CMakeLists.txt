# Unit tests for the engine wrapper, run against the stub engine.
qt_add_executable(tst_engine tst_engine.cpp)
target_link_libraries(tst_engine PRIVATE bayan_engine Qt6::Test bayan::compile_options)
qt_import_plugins(tst_engine EXCLUDE_BY_TYPE permissions) # See src/CMakeLists.txt.
add_test(NAME engine.wrapper COMMAND tst_engine)

# Unit tests for the document session (src/app), against the stub engine. The session's source is compiled into the test as well, because the
# application's QML module owns it.
qt_add_executable(tst_documentsession tst_documentsession.cpp
  "${PROJECT_SOURCE_DIR}/src/app/documentsession.cpp" "${PROJECT_SOURCE_DIR}/src/app/documentsession.h")
target_include_directories(tst_documentsession PRIVATE "${PROJECT_SOURCE_DIR}/src/app")
target_link_libraries(tst_documentsession PRIVATE bayan_engine Qt6::Gui Qt6::Qml Qt6::Test bayan::compile_options)
qt_import_plugins(tst_documentsession EXCLUDE_BY_TYPE permissions) # See src/CMakeLists.txt.
add_test(NAME app.document_session COMMAND tst_documentsession)

# Headless smoke test: the application starts with the offscreen platform plugin, shows the stub engine's page, checks its pixels and exits.
add_test(NAME app.smoke COMMAND bayan-desktop --smoke-test)

# Every test runs without a screen and with Qt's standard settings, whatever the developer's environment says.
set_tests_properties(engine.wrapper app.document_session app.smoke PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1"
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
