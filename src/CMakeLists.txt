add_subdirectory(engine)

qt_add_executable(bayan-desktop WIN32 MACOSX_BUNDLE main.cpp)
target_include_directories(bayan-desktop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app")

# Theme.qml is a singleton (one shared object holding the colours); this must be set before qt_add_qml_module.
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# The QML module that holds the interface. qmlcachegen (never the commercial qmlsc) compiles its QML files ahead of time.
qt_add_qml_module(bayan-desktop
  URI Bayan.Desktop
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/Ribbon.qml
    qml/DocumentArea.qml
    qml/Theme.qml
  SOURCES
    app/documentsession.cpp
    app/documentsession.h
    app/smoketest.cpp
    app/smoketest.h
    app/tileimageprovider.cpp
    app/tileimageprovider.h
)

target_compile_definitions(bayan-desktop PRIVATE BAYAN_DESKTOP_VERSION="${PROJECT_VERSION}")
target_link_libraries(bayan-desktop PRIVATE bayan_engine Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 bayan::compile_options)
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
