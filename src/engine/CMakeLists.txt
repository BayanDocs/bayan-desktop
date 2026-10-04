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
  # The real engine, from the C SDK that bayan-core's CI publishes (CORE-007): include/bayan_ffi.h and a shared library named bayan_ffi.
  set(BAYAN_ENGINE_SDK_DIR "" CACHE PATH "Directory of the unpacked bayan-core C SDK (with include/ and lib/)")
  if(NOT IS_DIRECTORY "${BAYAN_ENGINE_SDK_DIR}")
    message(FATAL_ERROR "BAYAN_ENGINE=sdk needs BAYAN_ENGINE_SDK_DIR, the directory of the unpacked bayan-core C SDK (CORE-007).")
  endif()
  find_path(BAYAN_ENGINE_SDK_INCLUDE_DIR bayan_ffi.h PATHS "${BAYAN_ENGINE_SDK_DIR}/include" NO_DEFAULT_PATH REQUIRED)
  find_library(BAYAN_ENGINE_SDK_LIBRARY bayan_ffi PATHS "${BAYAN_ENGINE_SDK_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
  add_library(bayan_engine_c INTERFACE)
  target_include_directories(bayan_engine_c SYSTEM INTERFACE "${BAYAN_ENGINE_SDK_INCLUDE_DIR}")
  target_link_libraries(bayan_engine_c INTERFACE "${BAYAN_ENGINE_SDK_LIBRARY}")
else()
  message(FATAL_ERROR "BAYAN_ENGINE must be 'stub' or 'sdk', not '${BAYAN_ENGINE}'")
endif()
message(STATUS "Engine: ${BAYAN_ENGINE}")

add_library(bayan_engine STATIC engine.cpp engine.h)
target_include_directories(bayan_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(bayan_engine
  PUBLIC Qt6::Core Qt6::Gui
  PRIVATE bayan_engine_c bayan::compile_options)
