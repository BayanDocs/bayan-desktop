# A stand-in for bayan-core's C SDK (CORE-007): builds the stub engine as a shared library and installs it in the SDK layout described in
# src/engine/README.md, so that building bayan-desktop with BAYAN_ENGINE=sdk can be tested before the real SDK exists.
# scripts/test-engine-sdk.sh builds it, installs it and then builds and tests bayan-desktop against it; it is not part of the normal build.

cmake_minimum_required(VERSION 3.25...4.4)

project(BayanEngineSdkStandIn
  DESCRIPTION "The stub engine packaged like the bayan-core C SDK, for tests"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(desktop_dir "${CMAKE_CURRENT_SOURCE_DIR}/../..")
list(APPEND CMAKE_MODULE_PATH "${desktop_dir}/cmake")
include(BayanQtPolicy)
include(BayanCompilerOptions)

# The stub uses Qt's JSON classes; the real engine needs no Qt.
bayan_find_qt(COMPONENTS Core)
find_package(Threads REQUIRED)

add_library(bayan_ffi SHARED "${desktop_dir}/src/engine/stub/bayan_engine_stub.cpp" "${desktop_dir}/src/engine/stub/include/bayan_ffi.h")
target_include_directories(bayan_ffi PRIVATE "${desktop_dir}/src/engine/stub/include")
target_link_libraries(bayan_ffi PRIVATE Qt6::Core Threads::Threads bayan::compile_options)
if(WIN32)
  # MSVC exports nothing from a DLL unless told to; the .def file exports exactly the functions of bayan_ffi.h, as the real SDK does.
  target_sources(bayan_ffi PRIVATE bayan_ffi.def)
endif()
# The installed library keeps a run path to Qt's libraries, which the stub needs. On macOS, CMake gives the library the install name
# @rpath/libbayan_ffi.dylib, and on Linux the SONAME libbayan_ffi.so, as src/engine/README.md asks of the SDK.
set_target_properties(bayan_ffi PROPERTIES INSTALL_RPATH_USE_LINK_PATH ON)

install(TARGETS bayan_ffi
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)
install(FILES "${desktop_dir}/src/engine/stub/include/bayan_ffi.h" DESTINATION include)
