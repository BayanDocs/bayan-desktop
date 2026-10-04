# The engine integration layer: the C interface of the engine (from the stub or from the bayan-core SDK) and the C++ wrapper over it.

if(BAYAN_ENGINE STREQUAL "stub")
  # A test engine that implements the C interface with a fixed page; see stub/bayan_engine_stub.cpp.
  add_library(bayan_engine_c STATIC stub/bayan_engine_stub.cpp stub/include/bayan_ffi.h)
  target_include_directories(bayan_engine_c PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/stub/include")
  target_link_libraries(bayan_engine_c PRIVATE Qt6::Core bayan::compile_options)
  # The stub runs its own thread.
  find_package(Threads REQUIRED)
  target_link_libraries(bayan_engine_c PRIVATE Threads::Threads)
elseif(BAYAN_ENGINE STREQUAL "sdk")
  # The real engine from bayan-core's C SDK (CORE-007), linked dynamically. README.md in this directory describes the SDK layout this expects.
  set(BAYAN_ENGINE_SDK_DIR "" CACHE PATH "Directory of the unpacked bayan-core C SDK (layout: src/engine/README.md)")
  if(NOT IS_DIRECTORY "${BAYAN_ENGINE_SDK_DIR}")
    message(FATAL_ERROR "BAYAN_ENGINE=sdk needs BAYAN_ENGINE_SDK_DIR, the directory of the unpacked bayan-core C SDK (CORE-007).")
  endif()
  find_path(sdk_include_dir bayan_ffi.h PATHS "${BAYAN_ENGINE_SDK_DIR}/include" NO_DEFAULT_PATH NO_CACHE REQUIRED)
  add_library(bayan_engine_c SHARED IMPORTED GLOBAL)
  set_target_properties(bayan_engine_c PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${sdk_include_dir}")
  if(WIN32)
    # The DLL, and the import library that the linker needs (Cargo names it bayan_ffi.dll.lib; both names are accepted).
    find_file(sdk_runtime bayan_ffi.dll PATHS "${BAYAN_ENGINE_SDK_DIR}/bin" NO_DEFAULT_PATH NO_CACHE REQUIRED)
    find_file(sdk_import_library NAMES bayan_ffi.lib bayan_ffi.dll.lib PATHS "${BAYAN_ENGINE_SDK_DIR}/lib" NO_DEFAULT_PATH NO_CACHE REQUIRED)
    set_target_properties(bayan_engine_c PROPERTIES IMPORTED_LOCATION "${sdk_runtime}" IMPORTED_IMPLIB "${sdk_import_library}")
  else()
    # libbayan_ffi.so or libbayan_ffi.dylib. CMake records the library's directory in the run path of the programs it builds, so they find the
    # library where the SDK is unpacked (on macOS only if the library's install name starts with @rpath/, as the README asks).
    find_file(sdk_runtime "${CMAKE_SHARED_LIBRARY_PREFIX}bayan_ffi${CMAKE_SHARED_LIBRARY_SUFFIX}" PATHS "${BAYAN_ENGINE_SDK_DIR}/lib"
      NO_DEFAULT_PATH NO_CACHE REQUIRED)
    set_target_properties(bayan_engine_c PROPERTIES IMPORTED_LOCATION "${sdk_runtime}")
  endif()
  message(STATUS "Engine SDK library: ${sdk_runtime}")
else()
  message(FATAL_ERROR "BAYAN_ENGINE must be 'stub' or 'sdk', not '${BAYAN_ENGINE}'")
endif()
message(STATUS "Engine: ${BAYAN_ENGINE}")

add_library(bayan_engine STATIC engine.cpp engine.h)
target_include_directories(bayan_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(bayan_engine
  PUBLIC Qt6::Core Qt6::Gui
  PRIVATE bayan_engine_c bayan::compile_options)

# Windows finds DLLs next to the program (or on PATH), so programs that use an SDK engine get a copy of its DLL; Linux and macOS use the run
# path recorded above. Only the engine's DLL is copied: Qt's DLLs stay where Qt is installed, so that Qt keeps finding its plugins.
function(bayan_deploy_engine_runtime target)
  if(WIN32 AND BAYAN_ENGINE STREQUAL "sdk")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:bayan_engine_c>" "$<TARGET_FILE_DIR:${target}>"
      COMMENT "Copying the engine's DLL next to ${target}"
      VERBATIM)
  endif()
endfunction()
