add_subdirectory(engine)

# The shell's C++ glue and its QML interface (module Bayan.Desktop), as a static library that the application and the tests link: the
# document session, the document canvas with its tile thread, input and input-method bridge (DESK-002), and the accessibility bridge.
qt_add_library(bayan_shell STATIC)

# Theme.qml is a singleton (one shared object holding the colours); this must be set before qt_add_qml_module.
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# The QML module that holds the interface. qmlcachegen (never the commercial qmlsc) compiles its QML files ahead of time. Its build output
# goes to a directory named after its URI, where qmllint and other QML tools look for it.
qt_add_qml_module(bayan_shell
  URI Bayan.Desktop
  VERSION 1.0
  OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Bayan/Desktop"
  QML_FILES
    qml/Main.qml
    qml/Ribbon.qml
    qml/DocumentArea.qml
    qml/Theme.qml
  SOURCES
    a11y/accessibletext.cpp
    a11y/accessibletext.h
    a11y/canvasaccessible.cpp
    a11y/canvasaccessible.h
    app/documentsession.cpp
    app/documentsession.h
    app/pixelprobe.cpp
    app/pixelprobe.h
    canvas/canvasnode.cpp
    canvas/canvasnode.h
    canvas/documentcanvas.cpp
    canvas/documentcanvas.h
    canvas/imetranslator.cpp
    canvas/imetranslator.h
    canvas/keymapping.cpp
    canvas/keymapping.h
    canvas/tilecache.cpp
    canvas/tilecache.h
    canvas/tilegrid.cpp
    canvas/tilegrid.h
    canvas/tilerenderer.cpp
    canvas/tilerenderer.h
    canvas/viewgeometry.cpp
    canvas/viewgeometry.h
)
target_include_directories(bayan_shell PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/a11y" "${CMAKE_CURRENT_SOURCE_DIR}/app" "${CMAKE_CURRENT_SOURCE_DIR}/canvas")
target_link_libraries(bayan_shell
  PUBLIC bayan_engine Qt6::Gui Qt6::Qml Qt6::Quick
  PRIVATE bayan::compile_options)

qt_add_executable(bayan-desktop WIN32 MACOSX_BUNDLE
  main.cpp
  app/benchmark.cpp
  app/benchmark.h
  app/smoketest.cpp
  app/smoketest.h
)
target_compile_definitions(bayan-desktop PRIVATE BAYAN_DESKTOP_VERSION="${PROJECT_VERSION}")
# bayan_shellplugin is the QML module's static plugin; main.cpp imports it with Q_IMPORT_QML_PLUGIN.
target_link_libraries(bayan-desktop PRIVATE bayan_shell bayan_shellplugin Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
                                            bayan::compile_options)
# On Apple platforms Qt can link its permission plugins (camera, microphone, location and so on) into the app as static libraries. The app
# needs none of those permissions, and Qt must be linked dynamically (ADR-0013), so they are excluded.
qt_import_plugins(bayan-desktop EXCLUDE_BY_TYPE permissions)
bayan_deploy_engine_runtime(bayan-desktop)

set_target_properties(bayan-desktop PROPERTIES
  OUTPUT_NAME bayan-desktop
  MACOSX_BUNDLE_BUNDLE_NAME "BayanDocs"
  MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.bayandocs.desktop"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
