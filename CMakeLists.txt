# bayan-desktop: the Qt Quick desktop shell of BayanDocs (ADR-0013).
# Build with the presets in CMakePresets.json; `cmake --workflow --preset verify` is the full verification gate (see AGENTS.md).

# 3.25 is required by Qt 6.12 and by workflow presets; CI and scripts/dev-setup.sh use the pinned CMake 4.4.3.
cmake_minimum_required(VERSION 3.25...4.4)

project(BayanDesktop
  VERSION 0.1.0
  DESCRIPTION "BayanDocs desktop application"
  HOMEPAGE_URL "https://github.com/BayanDocs/bayan-desktop"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(BayanQtPolicy)
include(BayanCompilerOptions)
include(BayanLint)

set(BAYAN_ENGINE "stub" CACHE STRING "Engine implementation to link: 'stub' (built-in test engine) or 'sdk' (the bayan-core C SDK from CORE-007)")
set_property(CACHE BAYAN_ENGINE PROPERTY STRINGS stub sdk)
option(BAYAN_BUILD_TESTS "Build the tests" ON)

set(qt_components Core Gui Qml Quick QuickControls2)
if(BAYAN_BUILD_TESTS)
  list(APPEND qt_components Test)
endif()
bayan_find_qt(COMPONENTS ${qt_components})
qt_standard_project_setup(REQUIRES 6.12)

add_subdirectory(src)

if(BAYAN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

file(GLOB_RECURSE lint_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/tests/*.cpp")
file(GLOB_RECURSE lint_headers CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.h" "${PROJECT_SOURCE_DIR}/tests/*.h")
set(lint_build_targets bayan-desktop)
if(BAYAN_BUILD_TESTS)
  list(APPEND lint_build_targets tst_engine tst_documentsession)
endif()
bayan_add_lint_targets(SOURCES ${lint_sources} HEADERS ${lint_headers} BUILD_TARGETS ${lint_build_targets})
