#!/usr/bin/env bash
# Installs the pinned build tools and Qt that bayan-desktop needs, and prints how to use them.
#
# The same script runs on developer machines, in BayanDocs cloud sessions and in CI, so that everyone builds with identical tool versions (ADR-0017):
#   - CMake, Ninja, clang-format and clang-tidy from PyPI, pinned exactly and checked against the SHA-256 hashes in deps/requirements-tools.txt (pip --require-hashes, wheels only, so no package code runs at install time);
#   - Qt at the exact version in deps/qt.json, installed by scripts/install-qt.py (Python standard library only), which checks every Qt archive against the SHA-256 hash published on download.qt.io. Only LGPL modules are installed (ADR-0013).
# It is idempotent: tools live in directories named after a hash of their pin files, so running it again does nothing unless a pin changed.
#
# Usage: scripts/dev-setup.sh [--link-dir DIR] [--no-qt]
#   --link-dir DIR  also link the tools into DIR (for example /usr/local/bin in a cloud environment's setup script)
#   --no-qt         do not install Qt (for example when you installed Qt 6.12.0 yourself; point CMAKE_PREFIX_PATH at it)
# Environment: BAYAN_TOOLS_DIR (where to install; default ~/.local/share/bayandocs/desktop-tools), BAYAN_QT_DIR (an existing Qt installation to use if it has the pinned version), PYTHON (Python 3.11 or newer).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tools_dir=${BAYAN_TOOLS_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/bayandocs/desktop-tools}
link_dir=""
want_qt=true

die() { echo "dev-setup: $*" >&2; exit 1; }
say() { echo "dev-setup: $*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --link-dir) [ $# -ge 2 ] || die "--link-dir needs a directory"; link_dir=$2; shift 2 ;;
    --no-qt) want_qt=false; shift ;;
    -h|--help) sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
done

case "$(uname -s)" in
  Linux*) host=linux ;;
  Darwin*) host=macos ;;
  MINGW*|MSYS*|CYGWIN*) host=windows ;;
  *) die "unsupported operating system: $(uname -s)" ;;
esac

python=${PYTHON:-}
if [ -z "$python" ]; then
  for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c 'import sys; sys.exit(sys.version_info < (3, 11))' 2>/dev/null; then
      python=$candidate
      break
    fi
  done
fi
[ -n "$python" ] || die "Python 3.11 or newer is required (set PYTHON to its path)"

# Reads one value from deps/qt.json: a top-level key, or a key of this operating system's entry under "hosts".
qt_pin() {
  "$python" - "$root/deps/qt.json" "$host" "$1" <<'EOF'
import json, sys
path, host, key = sys.argv[1:]
with open(path, encoding="utf-8") as f:
    pins = json.load(f)
value = pins[key] if key in pins else pins["hosts"][host][key]
print(" ".join(value) if isinstance(value, list) else value)
EOF
}

# Prints the first 16 hex digits of the SHA-256 of the given files (portable: no sha256sum on macOS).
digest() {
  "$python" -c 'import hashlib, sys; h = hashlib.sha256(); [h.update(open(p, "rb").read()) for p in sys.argv[1:]]; print(h.hexdigest()[:16])' "$@"
}

venv_bin() { if [ "$host" = windows ]; then echo "$1/Scripts"; else echo "$1/bin"; fi; }

# Creates a virtual environment with the hash-pinned requirements of one file, unless it already exists. Prints its path.
install_venv() {
  local name=$1 requirements=$2 venv
  venv="$tools_dir/$name-$(digest "$requirements")"
  if [ ! -f "$venv/.complete" ]; then
    say "installing $name from $(basename "$requirements") into $venv" >&2
    rm -rf -- "$venv"
    "$python" -m venv "$venv"
    "$(venv_bin "$venv")/python" -m pip install --quiet --no-input --disable-pip-version-check \
      --require-hashes --only-binary :all: -r "$requirements" >&2
    touch "$venv/.complete"
  fi
  echo "$venv"
}

# True if the directory is a Qt installation with the pinned version and the modules we use.
qt_matches() {
  local dir=$1 version=$2
  [ -n "$dir" ] && [ -f "$dir/lib/cmake/Qt6Quick/Qt6QuickConfig.cmake" ] &&
    cat "$dir/lib/cmake/Qt6Core/Qt6CoreConfigVersion"*.cmake 2>/dev/null | grep -q "set(PACKAGE_VERSION \"$version\")"
}

mkdir -p "$tools_dir"

tools_venv=$(install_venv tools "$root/deps/requirements-tools.txt")
tools_bin=$(venv_bin "$tools_venv")

qt_dir=""
if $want_qt; then
  qt_version=$(qt_pin qt)
  # An explicitly chosen Qt, the one the shell environment points at, or the one the BayanDocs cloud environment preinstalls.
  for candidate in "${BAYAN_QT_DIR:-}" "${QT_ROOT_DIR:-}" "/opt/Qt/$qt_version/$(qt_pin directory)"; do
    if qt_matches "$candidate" "$qt_version"; then qt_dir=$candidate; break; fi
  done
  if [ -z "$qt_dir" ]; then
    # Does nothing if this Qt is already installed; otherwise downloads it from Qt's servers and checks every archive's SHA-256.
    qt_dir="$tools_dir/Qt/$qt_version/$(qt_pin directory)"
    "$python" "$root/scripts/install-qt.py" --prefix "$tools_dir/Qt" --cmake "$tools_bin/cmake" --pins "$root/deps/qt.json" >&2 ||
      die "installing Qt $qt_version failed"
    qt_matches "$qt_dir" "$qt_version" || die "Qt $qt_version was not installed where expected ($qt_dir)"
  fi
fi

# Paths in the form the rest of the toolchain expects: native Windows paths there, unchanged elsewhere.
native() { if [ "$host" = windows ]; then cygpath -m "$1"; else echo "$1"; fi; }

env_file="$tools_dir/env.sh"
{
  echo "# Written by bayan-desktop/scripts/dev-setup.sh. Load with: . \"$env_file\""
  echo "export PATH=\"$tools_bin:\$PATH\""
  if [ -n "$qt_dir" ]; then
    echo "export QT_ROOT_DIR=\"$(native "$qt_dir")\""
    echo "export CMAKE_PREFIX_PATH=\"$(native "$qt_dir")\${CMAKE_PREFIX_PATH:+:\$CMAKE_PREFIX_PATH}\""
    # Windows finds DLLs through PATH; Linux and macOS use the run paths that CMake writes into the binaries.
    [ "$host" != windows ] || echo "export PATH=\"$qt_dir/bin:\$PATH\""
  fi
} >"$env_file"

# In GitHub Actions, make the tools and Qt available to the following steps.
if [ -n "${GITHUB_ACTIONS:-}" ]; then
  if [ "$host" = windows ]; then
    cygpath -w "$tools_bin" >>"$GITHUB_PATH"
    [ -z "$qt_dir" ] || cygpath -w "$qt_dir/bin" >>"$GITHUB_PATH"
  else
    echo "$tools_bin" >>"$GITHUB_PATH"
  fi
  if [ -n "$qt_dir" ]; then
    echo "QT_ROOT_DIR=$(native "$qt_dir")" >>"$GITHUB_ENV"
    echo "CMAKE_PREFIX_PATH=$(native "$qt_dir")" >>"$GITHUB_ENV"
  fi
fi

if [ -n "$link_dir" ]; then
  mkdir -p "$link_dir"
  for tool in cmake ctest cpack ninja clang-format clang-tidy; do
    ln -sfn "$tools_bin/$tool" "$link_dir/$tool"
  done
  say "linked cmake, ctest, cpack, ninja, clang-format and clang-tidy into $link_dir"
fi

say "tools in $tools_bin: $("$tools_bin/cmake" --version | head -n 1), ninja $("$tools_bin/ninja" --version), $("$tools_bin/clang-format" --version | sed 's/.*version /clang-format /'), $("$tools_bin/clang-tidy" --version | sed -n 's/.*LLVM version /clang-tidy /p')"
[ -z "$qt_dir" ] || say "Qt: $qt_dir"
say "to use them in this shell, run: . \"$env_file\""
