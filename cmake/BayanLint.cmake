# Lint targets, enabled with -DBAYAN_LINT=ON (the verify presets do this):
#   format-check  fails if any C++ file is not formatted as .clang-format says (format: reformats them in place)
#   tidy          runs clang-tidy with .clang-tidy on every C++ source file, treating findings as errors
# QML is linted by Qt's own all_qmllint target, which qt_add_qml_module creates.
#
# clang-format and clang-tidy must be exactly the versions pinned in deps/requirements-tools.txt, because different versions format and warn differently; scripts/dev-setup.sh installs them.

include_guard(GLOBAL)

option(BAYAN_LINT "Create the format-check and tidy targets (needs the pinned clang-format and clang-tidy)" OFF)

# Returns the version pinned for a Python package in deps/requirements-tools.txt.
function(_bayan_pinned_tool_version package out_var)
  file(STRINGS "${PROJECT_SOURCE_DIR}/deps/requirements-tools.txt" lines REGEX "^${package}==")
  if(NOT lines MATCHES "^${package}==([0-9.]+)")
    message(FATAL_ERROR "deps/requirements-tools.txt does not pin ${package}")
  endif()
  set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# Finds a tool and checks that it is exactly the pinned version.
function(_bayan_find_pinned_tool package out_var)
  _bayan_pinned_tool_version(${package} version)
  find_program(${out_var} NAMES ${package} DOC "${package} ${version} (from scripts/dev-setup.sh)")
  if(NOT ${out_var})
    message(FATAL_ERROR "BAYAN_LINT needs ${package} ${version}. Install the pinned tools with scripts/dev-setup.sh and load the environment it prints.")
  endif()
  execute_process(COMMAND "${${out_var}}" --version OUTPUT_VARIABLE output RESULT_VARIABLE result)
  string(REPLACE "." "\\." version_pattern "${version}")
  if(NOT result EQUAL 0 OR NOT output MATCHES "version ${version_pattern}([^0-9.]|$)")
    string(STRIP "${output}" output)
    message(FATAL_ERROR "BAYAN_LINT needs ${package} ${version}, but ${${out_var}} reports: ${output}\nInstall the pinned tools with scripts/dev-setup.sh and load the environment it prints (or clear ${out_var} in the CMake cache).")
  endif()
endfunction()

# Creates the lint targets for the given C++ files. <build_targets> are built first, because clang-tidy needs the files that Qt generates (moc, QML type registrations).
function(bayan_add_lint_targets)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "" "SOURCES;HEADERS;BUILD_TARGETS")
  if(NOT BAYAN_LINT)
    return()
  endif()

  _bayan_find_pinned_tool(clang-format BAYAN_CLANG_FORMAT)
  _bayan_find_pinned_tool(clang-tidy BAYAN_CLANG_TIDY)

  set(all_files ${arg_SOURCES} ${arg_HEADERS})
  add_custom_target(format-check
    COMMAND "${BAYAN_CLANG_FORMAT}" --dry-run --Werror ${all_files}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Checking C++ formatting with clang-format"
    VERBATIM)
  add_custom_target(format
    COMMAND "${BAYAN_CLANG_FORMAT}" -i ${all_files}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Formatting C++ files with clang-format"
    VERBATIM)

  # One command per source file, so that Ninja runs clang-tidy in parallel and only again when something changed.
  # clang-tidy also reads .clang-tidy files in subdirectories (tests/ has one); a change to any of them reruns it.
  file(GLOB_RECURSE tidy_configs CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/.clang-tidy" "${PROJECT_SOURCE_DIR}/tests/.clang-tidy")
  list(APPEND tidy_configs "${PROJECT_SOURCE_DIR}/.clang-tidy")
  set(stamps "")
  foreach(source IN LISTS arg_SOURCES)
    file(RELATIVE_PATH relative "${PROJECT_SOURCE_DIR}" "${source}")
    set(stamp "${PROJECT_BINARY_DIR}/tidy/${relative}.stamp")
    add_custom_command(
      OUTPUT "${stamp}"
      COMMAND "${BAYAN_CLANG_TIDY}" -p "${PROJECT_BINARY_DIR}" --quiet --warnings-as-errors=*
        # The compile commands may contain GCC-only options that Clang does not know.
        --extra-arg=-Wno-unknown-warning-option --extra-arg=-Wno-unused-command-line-argument
        "${source}"
      COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
      DEPENDS "${source}" ${arg_HEADERS} ${tidy_configs}
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      COMMENT "clang-tidy ${relative}"
      VERBATIM)
    list(APPEND stamps "${stamp}")
  endforeach()
  add_custom_target(tidy DEPENDS ${stamps})
  if(arg_BUILD_TARGETS)
    add_dependencies(tidy ${arg_BUILD_TARGETS})
  endif()
endfunction()
