#!/bin/sh
# Installs the Ubuntu packages of a Linux CI job exactly as a lock file lists them, on a base image pinned by digest (BayanDocs work
# package X-005; ADR-0017, amendment of 2026-11-05). The same file, byte for byte, is in bayan-desktop and bayan-web: what differs between
# them is only their lock file and their workflows. POSIX shell, clean under ShellCheck.
#
#   lock     For the monthly dependency session and for CI's check of the lock. Runs apt in the base image with Docker, against Ubuntu's
#            archive as it was at the snapshot date (apt-get update --snapshot, https:// sources), and writes the lock: the image with its
#            digest, its creation time, the architecture (amd64), the snapshot date and the requested packages, then apt's plan, one step
#            per line: each package file to unpack, with its path in Ubuntu's archive (the pool), its size and its SHA-256, and each
#            package to configure, in apt's order. The plan installs the requested packages without their recommendations and upgrades
#            the image's own packages that are newer at that date. apt checks the snapshot's index against Ubuntu's signature with the
#            image's own keyring and stops if that fails; the paths, sizes and hashes are taken only from that index (apt-cache show).
#            It refuses a snapshot date before the image's creation or not before the cutoff, a cutoff less than 24 hours ago, and a plan
#            that would remove a package. The same inputs always give the same file, byte for byte; --from reads them from a lock.
#   fetch    For CI, before the install: puts every file of the lock into a folder, which CI keeps in its cache. A file already there is
#            checked against the lock's size and SHA-256, and one that does not match stops the job. A missing file is downloaded from
#            the snapshot at the lock's date, else from Launchpad's librarian, else from the live archive: over HTTPS only, redirects
#            included, each download with time limits. The next source is tried only after a failed download, never after a size or
#            SHA-256 mismatch, which stops the job. A file is moved into the folder only after its check; several download at a time.
#   install  For CI, after the cache is saved: checks every file again, replays the plan with dpkg (a run of unpack steps as one
#            `dpkg --unpack`, a run of configure steps as one `dpkg --configure`), without apt and without network, ends with
#            `dpkg --configure --pending`, checks that `dpkg --audit` reports nothing and that every locked package is installed at its
#            locked version, and prints a summary (also into the job summary of GitHub Actions).
#
# The packages' maintainer scripts run while dpkg installs them, as they would under apt: the one exception to ADR-0017 rule 6 that the
# amendment names, limited to exactly the locked files.
#
# Usage:
#   ubuntu-packages.sh lock --cutoff <time> [--from <lock>] [--image <image>@sha256:<digest>] [--snapshot <date>] [--output <file>] [<package>...]
#   ubuntu-packages.sh fetch [--report <file>] <lock> <folder>
#   ubuntu-packages.sh install [--report <file>] <lock> <folder>
# <time> is a UTC time such as 2026-11-11T15:00:00Z (the session's cutoff: when it started, minus 24 hours); <date> is a snapshot ID such
# as 20261111T000000Z. Packages given to lock replace those of --from; --image and --snapshot replace its image and date. Without
# --output, lock writes the lock to standard output. fetch --report writes where each file came from into <file>, and install --report
# reads it for its summary.
#
# Environment:
#   DOCKER                      the Docker client that lock runs (default: docker)
#   https_proxy, HTTPS_PROXY    when set, lock runs the image on the host's network and apt connects through this proxy, and
#   CURL_CA_BUNDLE              apt also trusts the certificates in this file, for a proxy that intercepts TLS as in a BayanDocs cloud
#                               session (fetch's curl reads both variables itself). Neither can make apt accept an index without
#                               Ubuntu's signature, which apt checks with the image's own keyring.
#   UBUNTU_PACKAGES_SNAPSHOT_URL, UBUNTU_PACKAGES_LAUNCHPAD_URL, UBUNTU_PACKAGES_ARCHIVE_URL, UBUNTU_PACKAGES_STALL_SECONDS
#                               for the tests: the three sources' https:// addresses and the stall limit of a download, in seconds
set -eu

LC_ALL=C
export LC_ALL

me=ubuntu-packages
self=$0

# The sources of fetch, in the order it tries them. Every file is checked against the lock, so the source does not matter.
snapshot_url=${UBUNTU_PACKAGES_SNAPSHOT_URL:-https://snapshot.ubuntu.com/ubuntu}
launchpad_url=${UBUNTU_PACKAGES_LAUNCHPAD_URL:-https://launchpad.net/ubuntu/+archive/primary/+files}
archive_url=${UBUNTU_PACKAGES_ARCHIVE_URL:-https://archive.ubuntu.com/ubuntu}
# A download fails when it has not connected within 20 seconds, when it has moved less than 1 KB per second for this many seconds,
# or when it has taken 300 seconds in all.
stall_seconds=${UBUNTU_PACKAGES_STALL_SECONDS:-30}
# How many files fetch downloads at a time.
parallel_downloads=8

work=""
cleanup() {
  if [ -n "$work" ]; then
    rm -rf "$work"
  fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

say() { printf '%s: %s\n' "$me" "$*" >&2; }
die() {
  printf '%s: error: %s\n' "$me" "$*" >&2
  exit 1
}
usage() {
  awk 'NR == 1 { next } /^# Usage:/ { show = 1 } show && /^#/ { sub(/^# ?/, ""); print } show && !/^#/ { exit }' "$self" >&2
  exit 2
}
new_work_folder() { work=$(mktemp -d); }

# Prints a UTC time (2026-11-11T15:00:00Z, with or without fractions of a second) or a snapshot ID (20261111T000000Z) as
# "YYYYMMDDHHMMSS FFFFFFFFF", the fraction in nanoseconds, which sorts as text in the order of time. Prints nothing for anything else.
time_digits() {
  printf '%s\n' "$1" | awk '
    function two(s, low, high) { return s ~ /^[0-9][0-9]$/ && s + 0 >= low && s + 0 <= high }
    NR == 1 {
      if (length($0) == 16 && substr($0, 9, 1) == "T" && $0 ~ /^[0-9]+T[0-9]+Z$/) {
        d = substr($0, 1, 8) substr($0, 10, 6); f = ""
      } else if ($0 ~ /^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9](\.[0-9]+)?Z$/) {
        d = substr($0, 1, 4) substr($0, 6, 2) substr($0, 9, 2) substr($0, 12, 2) substr($0, 15, 2) substr($0, 18, 2)
        f = substr($0, 21); sub(/Z$/, "", f)
      } else exit
      if (!two(substr(d, 5, 2), 1, 12) || !two(substr(d, 7, 2), 1, 31) || !two(substr(d, 9, 2), 0, 23) || !two(substr(d, 11, 2), 0, 59) || !two(substr(d, 13, 2), 0, 59) || length(f) > 9) exit
      while (length(f) < 9) f = f "0"
      print d " " f
    }'
}

# True if the time $1 is before the time $2 (both as time_digits prints them).
before() { awk -v a="$1" -v b="$2" 'BEGIN { exit !((a "") < (b "")) }'; }

# Seconds since 1970 of a time in the form time_digits prints (whole seconds), computed without date, whose options differ between
# systems: the days since 1970 of the proleptic Gregorian calendar, as in Howard Hinnant's days_from_civil.
epoch_seconds() {
  awk -v t="$1" 'BEGIN {
    y = substr(t, 1, 4) + 0; m = substr(t, 5, 2) + 0; d = substr(t, 7, 2) + 0
    if (m <= 2) y -= 1
    era = int(y / 400); yoe = y - era * 400
    doy = int((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5) + d - 1
    doe = yoe * 365 + int(yoe / 4) - int(yoe / 100) + doy
    days = era * 146097 + doe - 719468
    printf "%d\n", days * 86400 + substr(t, 9, 2) * 3600 + substr(t, 11, 2) * 60 + substr(t, 13, 2)
  }'
}

# Checks every line of a lock file and prints its plan: "unpack <package> <version> <architecture> <pool path> <size> <sha256>" and
# "configure <package> <version>", in order. Stops at the first line that is not as the lock command writes it.
lock_plan() {
  awk '
    function bad(message) { print FILENAME ":" FNR ": " message | "cat 1>&2"; failed = 1; exit 1 }
    function expect(key) { if ($1 != key || NF != 2) bad("expected the line \"" key " <value>\"") }
    function digits(s, n) { return length(s) == n && s ~ /^[0-9]+$/ }
    function hex(s, n) { return length(s) == n && s ~ /^[0-9a-f]+$/ }
    function package(s) { return s ~ /^[a-z0-9][a-z0-9.+-]+$/ }
    function version(s) { return s ~ /^[0-9A-Za-z.+~:-]+$/ && s ~ /^[0-9]/ }
    BEGIN { state = "format" }
    /^#/ || /^[ \t]*$/ { next }
    state == "format" { if ($0 != "format 1") bad("the first line must be \"format 1\""); state = "image"; next }
    state == "image" {
      expect("image"); n = split($2, part, "@sha256:")
      if (n != 2 || part[1] !~ /^[a-z0-9][a-z0-9._\/:-]*$/ || !hex(part[2], 64)) bad("not an image with a digest: " $2)
      state = "created"; next
    }
    state == "created" {
      expect("created")
      if ($2 !~ /^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9](\.[0-9]+)?Z$/) bad("not a UTC time: " $2)
      state = "architecture"; next
    }
    state == "architecture" { expect("architecture"); if ($2 != "amd64") bad("the architecture must be amd64"); state = "snapshot"; next }
    state == "snapshot" {
      expect("snapshot"); split($2, s, "T")
      if (!digits(s[1], 8) || length($2) != 16 || substr($2, 9, 1) != "T" || !digits(substr($2, 10, 6), 6) || substr($2, 16) != "Z") bad("not a snapshot ID such as 20261111T000000Z: " $2)
      state = "request"; next
    }
    state == "request" && $1 == "request" {
      if (NF != 2 || !package($2)) bad("not a package name: " $0)
      if (requests && $2 <= last) bad("the requested packages must be sorted and named once: " $2)
      last = $2; requests++; next
    }
    state == "request" { if (!requests) bad("no requested packages"); state = "plan" }
    $1 == "unpack" {
      if (NF != 7) bad("expected \"unpack <package> <version> <architecture> <pool path> <size> <sha256>\"")
      if (!package($2) || !version($3) || ($4 != "amd64" && $4 != "all")) bad("malformed package, version or architecture")
      file = $5; sub(/^.*\//, "", file); plain = $3; sub(/^[0-9]+:/, "", plain)
      if ($5 !~ /^pool\/[a-z0-9-]+\/[a-z0-9+.-]+\/[a-z0-9+.-]+\/[A-Za-z0-9+.~_-]+\.deb$/ || file != $2 "_" plain "_" $4 ".deb") bad("not the pool path of this package: " $5)
      if (index("/" $5 "/", "/./") || index("/" $5 "/", "/../")) bad("a pool path with a \".\" or \"..\" directory: " $5)
      if ($6 !~ /^[1-9][0-9]*$/ || !hex($7, 64)) bad("malformed size or SHA-256")
      if ($2 in unpacked) bad("the package is unpacked twice: " $2)
      unpacked[$2] = $3; units++; print; next
    }
    $1 == "configure" {
      if (NF != 3) bad("expected \"configure <package> <version>\"")
      if (!($2 in unpacked) || unpacked[$2] != $3) bad("configures a package that is not unpacked before at this version: " $2)
      if ($2 in configured) bad("the package is configured twice: " $2)
      configured[$2] = 1; print; next
    }
    { bad("unexpected line") }
    END {
      if (failed) exit 1
      if (state != "plan" || !units) { print FILENAME ": the lock has no plan" | "cat 1>&2"; exit 1 }
      for (p in unpacked) if (!(p in configured)) { print FILENAME ": the package " p " is unpacked but never configured" | "cat 1>&2"; exit 1 }
    }
  ' "$1"
}

lock_value() { sed -n "s/^$2 //p" "$1"; }

# Prints nothing if the file $1 has the size $2 and the SHA-256 $3; otherwise says how it differs and fails.
file_problem() {
  actual=$(wc -c <"$1" | tr -d ' ')
  if [ "$actual" != "$2" ]; then
    echo "has $actual bytes, but the lock pins $2"
    return 1
  fi
  actual=$(sha256sum <"$1" | cut -d ' ' -f 1)
  if [ "$actual" != "$3" ]; then
    echo "has the SHA-256 $actual, but the lock pins $3"
    return 1
  fi
}

# ---------------------------------------------------------------------------------------------------------------------------- lock

# Runs inside the base image (lock runs the script there with sh -c). Prints "architecture <architecture>", the Inst, Conf, Remv and
# Purg lines of apt's simulated install, the line "index", and, from the verified index, the Package, Version, Architecture, Filename,
# Size and SHA256 fields of every package version that the plan unpacks.
plan_in_image() {
  snapshot=$1 proxy=$2 ca=$3
  shift 3
  DEBIAN_FRONTEND=noninteractive
  export DEBIAN_FRONTEND
  sources=/etc/apt/sources.list.d/ubuntu.sources
  [ -f "$sources" ] || die "the image has no $sources"
  # Ubuntu's archive only, and nothing that would let apt accept an index without Ubuntu's signature.
  for file in /etc/apt/sources.list.d/*; do
    [ "$file" = "$sources" ] || die "the image has another apt source, $file"
  done
  if [ -f /etc/apt/sources.list ] && grep -q '^[[:space:]]*[^#[:space:]]' /etc/apt/sources.list; then
    die "the image has apt sources in /etc/apt/sources.list"
  fi
  if grep -i -q -E 'trusted|allow-insecure|allow-weak|allow-downgrade' "$sources"; then
    die "$sources has an option that weakens the signature check"
  fi
  if apt-config dump | grep -i -q -E '^(Acquire::AllowInsecureRepositories|Acquire::AllowWeakRepositories|Acquire::AllowDowngradeToInsecureRepositories|APT::Get::AllowUnauthenticated) "(1|true|yes|on)"'; then
    die "the image configures apt to accept unsigned or weakly signed indexes"
  fi
  sed -i 's#^\(URIs:[[:space:]]*\)http://#\1https://#' "$sources"
  if grep '^URIs:' "$sources" | grep -v -q '^URIs:[[:space:]]*https://'; then
    die "every source in $sources must use https://"
  fi
  if [ -n "$proxy" ]; then
    printf 'Acquire::https::Proxy "%s";\n' "$proxy" >/etc/apt/apt.conf.d/99ubuntu-packages-proxy
  fi
  if [ -n "$ca" ]; then
    printf 'Acquire::https::CaInfo "%s";\n' "$ca" >>/etc/apt/apt.conf.d/99ubuntu-packages-proxy
  fi
  # --error-on=any: a source that cannot be fetched or verified fails the command instead of leaving a warning.
  apt-get -q update --error-on=any --snapshot "$snapshot" </dev/null >&2 ||
    die "apt-get update --snapshot $snapshot failed (see above): the snapshot could not be reached or its index could not be verified against Ubuntu's signature"
  printf 'architecture %s\n' "$(dpkg --print-architecture)"
  # Every installed package too, so that the plan also upgrades those that are newer at the snapshot date.
  # shellcheck disable=SC2016 # ${...} is dpkg-query's format, not the shell's
  installed=$(dpkg-query -W -f '${db:Status-Abbrev} ${Package}\n' | awk '$1 == "ii" { print $2 }')
  plan=$(mktemp)
  # shellcheck disable=SC2086 # $installed is a list of package names, one word each
  apt-get -s -q -o "APT::Snapshot=$snapshot" install --no-install-recommends "$@" $installed </dev/null >"$plan" ||
    die "apt could not plan the installation of: $*"
  grep -E '^(Inst|Conf|Remv|Purg) ' "$plan" || true
  echo index
  versions=$(awk '$1 == "Inst" { rest = $0; sub(/^[^(]*\(/, "", rest); split(rest, v, " "); print $2 "=" v[1] }' "$plan")
  if [ -n "$versions" ]; then
    # shellcheck disable=SC2086 # $versions is a list of package=version words
    apt-cache -o "APT::Snapshot=$snapshot" show $versions </dev/null | grep -E '^(Package|Version|Architecture|Filename|Size|SHA256): '
  fi
}

# Reads what plan_in_image printed and prints the lock's plan, or explains why it cannot.
lock_body() {
  awk '
    function bad(message) { print "apt'"'"'s plan cannot be locked: " message | "cat 1>&2"; failed = 1; exit 1 }
    function in_brackets(line) {
      if (!match(line, /\[[^]]*\]\)/)) bad("no architecture in: " line)
      return substr(line, RSTART + 1, RLENGTH - 3)
    }
    function new_version(line, rest, v) {
      rest = line; sub(/^[^(]*\(/, "", rest); split(rest, v, " "); return v[1]
    }
    function flush() {
      if (pkg == "") return
      key = pkg SUBSEP ver SUBSEP arch
      record = file " " size " " sha
      if ((key in index_record) && index_record[key] != record) bad("the index has two different files for " pkg " " ver)
      index_record[key] = record; pkg = ""
    }
    NR == 1 { if ($1 != "architecture" || NF != 2) bad("the image did not report its architecture"); architecture = $2; next }
    $0 == "index" { in_index = 1; next }
    !in_index && ($1 == "Remv" || $1 == "Purg") { removed = removed " " $2; next }
    !in_index && $1 == "Inst" {
      if ($2 in step_of) bad("apt unpacks " $2 " twice")
      steps++; op[steps] = "unpack"; name[steps] = $2; vers[steps] = new_version($0); archs[steps] = in_brackets($0); step_of[$2] = steps
      next
    }
    !in_index && $1 == "Conf" { steps++; op[steps] = "configure"; name[steps] = $2; vers[steps] = new_version($0); next }
    in_index && $1 == "Package:" { flush(); pkg = $2; ver = ""; arch = ""; file = ""; size = ""; sha = ""; next }
    in_index && $1 == "Version:" { ver = $2; next }
    in_index && $1 == "Architecture:" { arch = $2; next }
    in_index && $1 == "Filename:" { file = $2; next }
    in_index && $1 == "Size:" { size = $2; next }
    in_index && $1 == "SHA256:" { sha = $2; next }
    { bad("unexpected output from the image: " $0) }
    END {
      if (failed) exit 1
      flush()
      if (architecture != "amd64") bad("the image is " architecture ", not amd64")
      if (removed != "") bad("it would remove" removed)
      for (i = 1; i <= steps; i++) {
        if (op[i] == "unpack") {
          key = name[i] SUBSEP vers[i] SUBSEP archs[i]
          if (!(key in index_record)) bad("the verified index has no record of " name[i] " " vers[i] " (" archs[i] ")")
          print "unpack " name[i] " " vers[i] " " archs[i] " " index_record[key]
          unpacked[name[i]] = vers[i]
        } else {
          if (!(name[i] in unpacked) || unpacked[name[i]] != vers[i]) bad("it configures " name[i] " " vers[i] ", which it does not unpack before")
          if (name[i] in configured) bad("it configures " name[i] " twice")
          configured[name[i]] = 1
          print "configure " name[i] " " vers[i]
        }
      }
      for (p in unpacked) if (!(p in configured)) bad("it unpacks " p " but never configures it")
      if (!steps) bad("it installs nothing")
    }
  '
}

cmd_lock() {
  from="" image="" snapshot="" cutoff="" output=""
  while [ $# -gt 0 ]; do
    case $1 in
      --from | --image | --snapshot | --cutoff | --output)
        [ $# -ge 2 ] || usage
        case $1 in
          --from) from=$2 ;;
          --image) image=$2 ;;
          --snapshot) snapshot=$2 ;;
          --cutoff) cutoff=$2 ;;
          --output) output=$2 ;;
        esac
        shift 2
        ;;
      --) shift; break ;;
      -*) usage ;;
      *) break ;;
    esac
  done
  new_work_folder
  set -f
  if [ -n "$from" ]; then
    lock_plan "$from" >/dev/null || die "$from is not a lock file that this script wrote"
    [ -n "$image" ] || image=$(lock_value "$from" image)
    [ -n "$snapshot" ] || snapshot=$(lock_value "$from" snapshot)
    if [ $# -eq 0 ]; then
      # shellcheck disable=SC2046 # one package name per word
      set -- $(lock_value "$from" request)
    fi
  fi
  [ -n "$cutoff" ] || die "--cutoff is required: the time the session started, minus 24 hours (for example 2026-11-11T15:00:00Z)"
  [ -n "$image" ] || die "--image is required (or --from): the base image with its digest"
  [ -n "$snapshot" ] || die "--snapshot is required (or --from): a snapshot ID such as 20261111T000000Z"
  [ $# -gt 0 ] || die "name the packages to install (or use --from)"
  case $image in
    *@sha256:*) ;;
    *) die "the image must be named with its digest (<image>@sha256:<digest>): $image" ;;
  esac
  digest=${image#*@sha256:}
  if [ "${#digest}" -ne 64 ] || ! printf '%s\n' "$image" | grep -q -E '^[a-z0-9][a-z0-9._/:-]*@sha256:[0-9a-f]{64}$'; then
    die "not an image with a digest: $image"
  fi
  case $snapshot in
    ????????T??????Z) snapshot_time=$(time_digits "$snapshot") ;;
    *) snapshot_time="" ;;
  esac
  [ -n "$snapshot_time" ] || die "not a snapshot ID such as 20261111T000000Z: $snapshot"
  case $cutoff in
    ????-??-??T??:??:??Z) cutoff_time=$(time_digits "$cutoff") ;;
    *) cutoff_time="" ;;
  esac
  [ -n "$cutoff_time" ] || die "not a UTC time such as 2026-11-11T15:00:00Z: $cutoff"
  for package in "$@"; do
    printf '%s\n' "$package" | grep -q -E '^[a-z0-9][a-z0-9.+-]+$' || die "not a package name: $package"
  done
  printf '%s\n' "$@" | sort -u >"$work/requests"
  # The cutoff is the start of the session minus 24 hours (ADR-0017 rule 4), so it is at least 24 hours ago.
  [ $(($(epoch_seconds "$cutoff_time") + 86400)) -le "$(date -u +%s)" ] || die "the cutoff $cutoff is less than 24 hours ago"
  before "$snapshot_time" "$cutoff_time" || die "the snapshot $snapshot is not before the cutoff $cutoff"

  docker=${DOCKER:-docker}
  if ! "$docker" image inspect "$image" >/dev/null 2>&1; then
    say "pulling $image"
    "$docker" pull --platform linux/amd64 "$image" >&2 || die "could not pull $image"
  fi
  # shellcheck disable=SC2016 # {{...}} is Docker's template, not the shell's
  details=$("$docker" image inspect --format '{{.Created}} {{.Os}} {{.Architecture}}' "$image") || die "could not inspect $image"
  # shellcheck disable=SC2086 # three words
  set -- $details
  [ $# -eq 3 ] || die "unexpected details of $image: $details"
  created=$1
  [ "$2/$3" = linux/amd64 ] || die "$image is $2/$3 on this machine, not linux/amd64"
  created_time=$(time_digits "$created")
  [ -n "$created_time" ] || die "the creation time of $image is not a UTC time: $created"
  if before "$snapshot_time" "$created_time"; then
    die "the snapshot $snapshot is before the image was created ($created): a -dev package could then need an older version of its library than the image has"
  fi

  proxy=${https_proxy:-${HTTPS_PROXY:-}}
  ca=${CURL_CA_BUNDLE:-}
  if [ -n "$proxy" ]; then
    case $proxy in
      http://* | https://*) ;;
      *) die "the proxy address must start with http:// or https://: $proxy" ;;
    esac
    case $proxy in
      *[!A-Za-z0-9.:/@_-]*) die "unexpected characters in the proxy address" ;;
    esac
    set -- --network host
  else
    set --
    ca=""
  fi
  ca_in_image=""
  if [ -n "$ca" ]; then
    [ -f "$ca" ] || die "CURL_CA_BUNDLE names $ca, which is not a file"
    ca_in_image=/usr/local/share/ubuntu-packages-proxy-ca.crt
    set -- "$@" --volume "$ca:$ca_in_image:ro"
  fi
  say "running apt in $image at the snapshot $snapshot (it downloads Ubuntu's package index, about 70 MB)"
  # shellcheck disable=SC2046 # one package name per word
  "$docker" run --rm --platform linux/amd64 "$@" "$image" sh -c "$(cat "$self")" "$me" __plan "$snapshot" "$proxy" "$ca_in_image" \
    $(cat "$work/requests") >"$work/apt" || die "apt in the image failed (see above)"
  lock_body <"$work/apt" >"$work/body" || exit 1

  {
    echo "# Ubuntu packages for Linux CI jobs: exact files on a base image pinned by digest, installed by scripts/ubuntu-packages.sh (BayanDocs X-005)."
    echo "# Written by that script's lock command, which ran apt in the image below against Ubuntu's archive as it was at the snapshot date and"
    echo "# checked its index against Ubuntu's signature; do not edit by hand. To write it again, for example with a newer image or date:"
    echo "#   sh scripts/ubuntu-packages.sh lock --from <this file> --cutoff <session start minus 24 h> [--image <image>] [--snapshot <date>] --output <this file>"
    echo "# The plan is apt's, in its order: unpack <package> <version> <architecture> <pool path> <size> <SHA-256>, and configure <package> <version>."
    echo "format 1"
    echo "image $image"
    echo "created $created"
    echo "architecture amd64"
    echo "snapshot $snapshot"
    sed 's/^/request /' "$work/requests"
    cat "$work/body"
  } >"$work/lock"
  lock_plan "$work/lock" >/dev/null || die "the lock that was written does not check (a bug in this script)"
  set +f

  summary=$(awk '$1 == "unpack" { n++; bytes += $6 } $1 == "configure" { c++ } END { printf "%d files to unpack (%.1f MB), %d packages to configure", n, bytes / 1e6, c }' "$work/body")
  if [ -n "$output" ]; then
    cp "$work/lock" "$output"
    say "wrote $output: $summary"
  else
    cat "$work/lock"
    say "$summary"
  fi
}

# --------------------------------------------------------------------------------------------------------------------------- fetch

# Puts one file of the lock into the folder (run by fetch for each file, several at a time). Arguments: the folder, the folder for
# downloads in progress, the folder for results, the snapshot date, and the file's pool path, size and SHA-256.
fetch_file() {
  folder=$1 partial=$2 results=$3 snapshot=$4 pool=$5 size=$6 sha=$7
  name=${pool##*/}
  if [ -e "$folder/$name" ]; then
    if problem=$(file_problem "$folder/$name" "$size" "$sha"); then
      echo cache >"$results/$name"
      return 0
    fi
    say "$name in the cache $problem. The cache holds a file that does not match the lock, so the job stops here. To recover, change the version in the cache key of the workflow's cache steps (for example ubuntu-packages-v1 to ubuntu-packages-v2) in a pull request: its runs start with an empty cache, and once it is merged, the next run on main saves a new one."
    return 1
  fi
  if [ "$stall_seconds" -lt 20 ]; then connect_seconds=$stall_seconds; else connect_seconds=20; fi
  errors=""
  for source in snapshot launchpad archive; do
    case $source in
      snapshot) url=$snapshot_url/$snapshot/$pool ;;
      launchpad) url=$launchpad_url/$name ;;
      archive) url=$archive_url/$pool ;;
    esac
    host=${url#https://}
    host=${host%%/*}
    rm -f "$partial/$name"
    if curl --proto '=https' --proto-redir '=https' --tlsv1.2 --fail --silent --show-error --location --max-redirs 5 \
      --connect-timeout "$connect_seconds" --speed-limit 1024 --speed-time "$stall_seconds" --max-time 300 \
      --output "$partial/$name" "$url" 2>"$partial/$name.error"; then
      if problem=$(file_problem "$partial/$name" "$size" "$sha"); then
        mv "$partial/$name" "$folder/$name"
        echo "$source" >"$results/$name"
        say "$name: downloaded from $host"
        return 0
      fi
      say "$name from $url $problem. It may have been tampered with, so no other source is tried and the job stops here."
      return 1
    fi
    error=$(tr '\n' ' ' <"$partial/$name.error" | sed 's/ *$//')
    errors="$errors; $url: $error"
    if [ "$source" != archive ]; then
      say "$name: could not download from $url ($error); trying the next source"
    fi
  done
  say "could not download $name from any source: ${errors#; }"
  return 1
}

cmd_fetch() {
  report=""
  while [ $# -gt 0 ]; do
    case $1 in
      --report) [ $# -ge 2 ] || usage; report=$2; shift 2 ;;
      --) shift; break ;;
      -*) usage ;;
      *) break ;;
    esac
  done
  [ $# -eq 2 ] || usage
  lock=$1 folder=$2
  for url in "$snapshot_url" "$launchpad_url" "$archive_url"; do
    case $url in
      https://?*) ;;
      *) die "every source address must start with https://: $url" ;;
    esac
  done
  case $stall_seconds in
    '' | *[!0-9]* | 0*) die "UBUNTU_PACKAGES_STALL_SECONDS must be a whole number of seconds" ;;
  esac
  new_work_folder
  lock_plan "$lock" >"$work/plan" || die "$lock is not a lock file that this script wrote"
  snapshot=$(lock_value "$lock" snapshot)
  awk '$1 == "unpack" { print $5, $6, $7 }' "$work/plan" >"$work/files"
  mkdir -p "$folder" "$work/results"
  rm -rf "$folder"/.partial.*
  partial=$(mktemp -d "$folder/.partial.XXXXXX")
  start=$(date +%s)
  status=0
  xargs -L 1 -P "$parallel_downloads" sh "$self" __fetch-file "$folder" "$partial" "$work/results" "$snapshot" <"$work/files" || status=$?
  rm -rf "$partial"
  seconds=$(($(date +%s) - start))
  files=$(wc -l <"$work/files" | tr -d ' ')
  find "$work/results" -type f -exec cat {} + >"$work/sources"
  counts=$(awk '{ n[$1]++ } END { printf "%d %d %d %d %d", n["cache"] + n["snapshot"] + n["launchpad"] + n["archive"], n["cache"], n["snapshot"], n["launchpad"], n["archive"] }' "$work/sources")
  # shellcheck disable=SC2086 # five numbers
  set -- $counts
  if [ "$status" -ne 0 ] || [ "$1" -ne "$files" ]; then
    die "$((files - $1)) of the $files files of $lock could not be put into $folder (see above)"
  fi
  bytes=$(awk '$1 == "unpack" { n += $6 } END { printf "%.0f", n }' "$work/plan")
  if [ -n "$report" ]; then
    printf 'files %s\nbytes %s\ncache %s\nsnapshot %s\nlaunchpad %s\narchive %s\nseconds %s\n' "$files" "$bytes" "$2" "$3" "$4" "$5" "$seconds" >"$report"
  fi
  say "$files files in $folder, each checked against $lock: $2 from the cache, $3 from snapshot.ubuntu.com, $4 from Launchpad, $5 from archive.ubuntu.com, in $seconds s"
}

# ------------------------------------------------------------------------------------------------------------------------- install

# Runs dpkg on the batch of unpack or configure steps collected so far.
run_batch() {
  [ -n "$batch" ] || return 0
  calls=$((calls + 1))
  # shellcheck disable=SC2086 # $batch is a list of file or package names, one word each (checked by lock_plan)
  case $batch_op in
    unpack) (cd "$folder" && dpkg --unpack --auto-deconfigure --no-triggers --force-confdef --force-confold $batch) </dev/null ||
      die "dpkg --unpack failed (see above)" ;;
    configure) dpkg --configure --no-triggers --force-confdef --force-confold $batch </dev/null || die "dpkg --configure failed (see above)" ;;
  esac
  batch=""
}

cmd_install() {
  report=""
  while [ $# -gt 0 ]; do
    case $1 in
      --report) [ $# -ge 2 ] || usage; report=$2; shift 2 ;;
      --) shift; break ;;
      -*) usage ;;
      *) break ;;
    esac
  done
  [ $# -eq 2 ] || usage
  lock=$1 folder=$2
  [ "$(id -u)" -eq 0 ] || die "install runs dpkg, so it must run as root"
  command -v dpkg >/dev/null || die "dpkg is not installed"
  new_work_folder
  lock_plan "$lock" >"$work/plan" || die "$lock is not a lock file that this script wrote"
  start=$(date +%s)
  # Every file again, as the cache step may have read the folder since fetch checked it.
  while read -r op _ _ _ pool size sha; do
    [ "$op" = unpack ] || continue
    name=${pool##*/}
    [ -f "$folder/$name" ] || die "$name is not in $folder: run the fetch command first"
    problem=$(file_problem "$folder/$name" "$size" "$sha") || die "$name in $folder $problem"
  done <"$work/plan"

  DEBIAN_FRONTEND=noninteractive
  export DEBIAN_FRONTEND
  calls=0 batch="" batch_op=""
  set -f
  while read -r op package _ _ pool _; do
    case $op in
      unpack) item=${pool##*/} ;;
      *) item=$package ;;
    esac
    if [ "$op" != "$batch_op" ]; then
      run_batch
      batch_op=$op
    fi
    batch="$batch $item"
  done <"$work/plan"
  run_batch
  set +f
  dpkg --configure --pending </dev/null || die "dpkg --configure --pending failed (see above)"
  calls=$((calls + 1))
  audit=$(dpkg --audit 2>&1) || die "dpkg --audit failed: $audit"
  [ -z "$audit" ] || die "dpkg --audit reports unfinished packages: $audit"
  awk '$1 == "unpack" { print $2, $3 }' "$work/plan" >"$work/expected"
  set -f
  # shellcheck disable=SC2016,SC2046 # ${...} is dpkg-query's format; one package name per word
  dpkg-query -W -f '${Package} ${db:Status-Abbrev}${Version}\n' $(awk '{ print $1 }' "$work/expected") >"$work/installed" 2>&1 || true
  set +f
  # ($1 in state) before state[$1]: in awk, reading an element that is not there creates it.
  awk 'NR == FNR { state[$1] = $2 " " $3; next } { actual = ($1 in state) ? state[$1] : "not installed" }
       actual != "ii " $2 { print $1 " " $2 ": " actual; bad = 1 } END { exit bad }' "$work/installed" "$work/expected" >"$work/wrong" ||
    die "these packages are not installed at their locked versions: $(tr '\n' ';' <"$work/wrong")"
  seconds=$(($(date +%s) - start))
  packages=$(wc -l <"$work/expected" | tr -d ' ')

  fetched="where the files came from is not known (fetch was run without --report)"
  if [ -n "$report" ] && [ -f "$report" ]; then
    fetched=$(awk '{ v[$1] = $2 } END { printf "%d files (%.1f MB): %d from the cache, %d from snapshot.ubuntu.com, %d from Launchpad, %d from archive.ubuntu.com, fetched in %d s", v["files"], v["bytes"] / 1e6, v["cache"], v["snapshot"], v["launchpad"], v["archive"], v["seconds"] }' "$report")
  fi
  say "installed the $packages packages of $lock with $calls dpkg calls in $seconds s; dpkg --audit reports nothing. Package files: $fetched."
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
      echo "### Ubuntu packages"
      echo
      echo "Installed the $packages packages of \`$lock\` with $calls dpkg calls in $seconds s, without apt and without network; \`dpkg --audit\` reports nothing."
      echo
      echo "Package files: $fetched."
    } >>"$GITHUB_STEP_SUMMARY"
  fi
}

case ${1:-} in
  lock) shift; cmd_lock "$@" ;;
  fetch) shift; cmd_fetch "$@" ;;
  install) shift; cmd_install "$@" ;;
  __plan) shift; plan_in_image "$@" ;;
  __fetch-file) shift; fetch_file "$@" ;;
  -h | --help) usage ;;
  *) usage ;;
esac
