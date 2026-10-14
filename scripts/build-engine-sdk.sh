#!/usr/bin/env bash
# Builds the bayan-core C SDK (CORE-007) from the commit pinned in deps/bayan-core.json, so that the app can be tested against the real engine
# (BAYAN_ENGINE=sdk). Before bayan-core publishes releases, shells may build core from a pinned commit (ADR-0002 §4, Phase 0 only).
#   1. fetches exactly that commit into build/bayan-core, and checks that it got it and that the commit is on bayan-core's main branch (or
#      uses the checkout given with --core, to try local changes to bayan-core; it then says when that checkout is not at the pinned commit);
#   2. installs, with rustup, the Rust version that commit pins in its rust-toolchain.toml, in the minimal profile and without updating rustup;
#   3. builds the SDK with `cargo xtask sdk`, which builds with bayan-core's Cargo.lock (--locked), into build/bayan-core-sdk, in the layout
#      that src/engine/README.md describes.
# Then scripts/test-engine-sdk.sh --sdk build/bayan-core-sdk builds the app against it and runs the tests that use the engine. CI runs both on
# Linux, macOS and Windows. Needs git, rustup and Python 3. On Windows, run it from Git Bash outside the Visual Studio developer environment:
# there, Git Bash's own link.exe comes first on PATH and Rust would take it for the MSVC linker, which it otherwise finds by itself.
#
# Usage: scripts/build-engine-sdk.sh [--core <bayan-core checkout>]
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sdk_dir="$root/build/bayan-core-sdk"
python=${PYTHON:-python3}

core_dir=""
while (($# > 0)); do
  case "$1" in
    --core)
      core_dir=$(cd "${2:?--core needs a folder}" && pwd)
      shift 2
      ;;
    *)
      echo "Unknown argument: $1 (usage: scripts/build-engine-sdk.sh [--core <bayan-core checkout>])" >&2
      exit 2
      ;;
  esac
done

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) windows=true ;;
  *) windows=false ;;
esac
# Rust and CMake on Windows expect native paths.
native() { if $windows; then cygpath -m "$1"; else echo "$1"; fi; }

# The pin: a GitHub repository and a full commit hash, nothing else.
pin=$("$python" -c 'import json, sys; pin = json.load(open(sys.argv[1], encoding="utf-8")); print(pin["repository"], pin["commit"])' \
  "$(native "$root/deps/bayan-core.json")")
read -r repository commit <<<"$pin"
if [[ ! $repository =~ ^https://github\.com/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+\.git$ ]] || [[ ! $commit =~ ^[0-9a-f]{40}$ ]]; then
  echo "deps/bayan-core.json must name a https://github.com/<owner>/<repository>.git URL and a full 40-character commit hash" >&2
  exit 1
fi

if [[ -z $core_dir ]]; then
  core_dir="$root/build/bayan-core"
  if [[ ! -d $core_dir/.git ]]; then
    git init --quiet "$core_dir"
  fi
  git -C "$core_dir" fetch --quiet --depth 1 --no-tags "$repository" "$commit"
  git -C "$core_dir" -c advice.detachedHead=false checkout --quiet --force FETCH_HEAD
  if [[ $(git -C "$core_dir" rev-parse HEAD) != "$commit" ]]; then
    echo "Fetched $(git -C "$core_dir" rev-parse HEAD) instead of the pinned $commit" >&2
    exit 1
  fi
  # GitHub serves every commit of a repository's forks through the repository's own URL too, so getting the commit does not show that it
  # is the project's. The pin must name a commit on main: fetch main's history, commits only (no files), and check that it contains it.
  git -C "$core_dir" fetch --quiet --no-tags --filter=tree:0 "$repository" "+refs/heads/main:refs/bayan-pin/main"
  if ! git -C "$core_dir" merge-base --is-ancestor "$commit" refs/bayan-pin/main; then
    echo "The pinned commit $commit is not on the main branch of $repository" >&2
    exit 1
  fi
  echo "bayan-core $commit (pinned in deps/bayan-core.json, on main)"
elif [[ $(git -C "$core_dir" rev-parse HEAD) != "$commit" ]]; then
  echo "Note: $core_dir is at $(git -C "$core_dir" rev-parse HEAD), not at the commit pinned in deps/bayan-core.json ($commit)." >&2
fi

# The Rust version that bayan-core pins, only the compiler, Cargo and the standard library: building the SDK needs neither the WebAssembly
# targets nor Clippy that bayan-core's rust-toolchain.toml also lists.
toolchain=$(sed -n 's/^channel = "\([0-9][0-9.]*\)"$/\1/p' "$core_dir/rust-toolchain.toml")
if [[ ! $toolchain =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Could not read an exact Rust version from $core_dir/rust-toolchain.toml" >&2
  exit 1
fi
rustup toolchain install "$toolchain" --profile minimal --no-self-update

# A fresh SDK folder each time, so that nothing from an earlier build is picked up.
rm -rf -- "$sdk_dir"
(cd "$core_dir" && RUSTUP_TOOLCHAIN=$toolchain cargo xtask sdk --out "$(native "$sdk_dir")")
echo "The bayan-core C SDK is in $sdk_dir"
