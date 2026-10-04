#!/usr/bin/env bash
# Tests building bayan-desktop against the bayan-core C SDK (BAYAN_ENGINE=sdk) before the real SDK exists (CORE-007):
#   1. builds the stub engine as a shared library with tests/engine-sdk and installs it as a stand-in SDK, in the layout of src/engine/README.md;
#   2. builds bayan-desktop against it and runs the tests that use the engine (workflow preset engine-sdk, or engine-sdk-msvc on Windows).
# CI runs it on Linux, macOS and Windows. Run it from any directory after loading the environment that scripts/dev-setup.sh prints; on Windows,
# run it from Git Bash inside an x64 Visual Studio developer environment, as for the msvc preset.
#
# Usage: scripts/test-engine-sdk.sh
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
work_dir="$root/build/stand-in-sdk"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) windows=true ;;
  *) windows=false ;;
esac
# CMake on Windows expects native paths.
native() { if $windows; then cygpath -m "$1"; else echo "$1"; fi; }

configure=(-S "$(native "$root/tests/engine-sdk")" -B "$(native "$work_dir/build")" -G Ninja -D CMAKE_BUILD_TYPE=RelWithDebInfo)
if $windows; then
  configure+=(-D CMAKE_CXX_COMPILER=cl)
  preset=engine-sdk-msvc
else
  preset=engine-sdk
fi

cmake "${configure[@]}"
cmake --build "$(native "$work_dir/build")"
# A fresh SDK directory each time, so that nothing from an earlier run is picked up.
rm -rf -- "$work_dir/sdk"
cmake --install "$(native "$work_dir/build")" --prefix "$(native "$work_dir/sdk")"

BAYAN_ENGINE_SDK_DIR=$(native "$work_dir/sdk")
export BAYAN_ENGINE_SDK_DIR
cd "$root"
cmake --workflow --preset "$preset"
