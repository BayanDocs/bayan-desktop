# Compares the copies of the C SDK's header and JSON Schema that bayan-desktop keeps in src/engine/stub (COPY_DIR) with the SDK's own (SDK_DIR),
# byte for byte: include/bayan_ffi.h and schema/engine-protocol.schema.json. The test engine.sdk_copies (tests/CMakeLists.txt) runs it
# whenever the app is built against an SDK; in CI that is the SDK built from the bayan-core commit pinned in deps/bayan-core.json, so moving
# the pin without updating the copies fails.
#
# Usage: cmake -D SDK_DIR=<SDK> -D COPY_DIR=<src/engine/stub> -P compare-copies.cmake

foreach(variable IN ITEMS SDK_DIR COPY_DIR)
  if(NOT IS_DIRECTORY "${${variable}}")
    message(FATAL_ERROR "${variable} must name a directory, not '${${variable}}'")
  endif()
endforeach()

set(differences "")
foreach(file IN ITEMS include/bayan_ffi.h schema/engine-protocol.schema.json)
  if(NOT EXISTS "${SDK_DIR}/${file}")
    list(APPEND differences "${file} is missing from the SDK")
  elseif(NOT EXISTS "${COPY_DIR}/${file}")
    list(APPEND differences "${file} is missing from ${COPY_DIR}")
  else()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${SDK_DIR}/${file}" "${COPY_DIR}/${file}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
      list(APPEND differences "${file} differs")
    endif()
  endif()
endforeach()

if(differences)
  list(JOIN differences "; " summary)
  message(FATAL_ERROR
    "The copies in ${COPY_DIR} are not those of the SDK in ${SDK_DIR}: ${summary}.\n"
    "When the bayan-core pin in deps/bayan-core.json moves, copy include/bayan_ffi.h and schema/engine-protocol.schema.json from the SDK that "
    "scripts/build-engine-sdk.sh builds from the new commit into src/engine/stub/include and src/engine/stub/schema, unchanged, and adapt the "
    "stub and the tests to what changed (src/engine/README.md).")
endif()
message(STATUS "include/bayan_ffi.h and schema/engine-protocol.schema.json in ${COPY_DIR} are identical to the SDK's.")
