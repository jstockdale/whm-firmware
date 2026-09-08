#!/usr/bin/env bash
#
# verify.sh — build + verify the wh-link protocol library end to end.
#
# Fail-fast: every dependency is checked up front (with install hints) before
# any build runs; any stage failure exits non-zero immediately. Zero warnings
# are enforced via -Werror on all C builds.
#
# Stages:
#   1. Reference crypto KATs        (RFC/NIST vectors)        -Og and -O2
#   2. Protocol tests               (framing/pairing/etc.)    -Og and -O2
#   3. Cross-check vs pyca          (independent oracle)
#   4. Header-only split            (one impl TU + one user TU) links + runs
#   5. C++ includability            (C++ TU links vs C impl; impl compiles C++)
#
set -euo pipefail
cd "$(dirname "$0")"

STRICT="-std=c11 -Wall -Wextra -Wformat-truncation=2 -Werror=format-truncation -Wmisleading-indentation -Wmissing-field-initializers -Werror"
CXXSTRICT="-std=c++17 -Wall -Wextra -Werror"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
fail()  { red "FAIL: $*"; exit 1; }

# ------------------------------------------------------------------ deps
echo "== dependency check =="
missing=0
need() { command -v "$1" >/dev/null 2>&1 || { red "missing: $1 — $2"; missing=1; }; }
need gcc     "install: sudo apt-get install build-essential"
need g++     "install: sudo apt-get install build-essential"
need python3 "install: sudo apt-get install python3"
if command -v python3 >/dev/null 2>&1; then
  python3 -c 'import cryptography' 2>/dev/null || {
    red "missing: python module 'cryptography' — install: python3 -m pip install cryptography"
    missing=1
  }
fi
[ "$missing" -eq 0 ] || { red "dependency check failed; aborting before any build."; exit 1; }
green "deps OK"

BIN="$(mktemp -d)"
trap 'rm -rf "$BIN"' EXIT

# ---------------------------------------------------------- 1. crypto KATs
echo "== [1] reference crypto KATs =="
for opt in -Og -O2; do
  gcc $STRICT $opt -o "$BIN/kat" test_crypto_kat.c || fail "crypto KAT compile $opt"
  "$BIN/kat" >/dev/null            || fail "crypto KAT run $opt"
  echo "   crypto KATs pass ($opt)"
done

# --------------------------------------------------------- 2. protocol tests
echo "== [2] protocol tests =="
for opt in -Og -O2; do
  gcc $STRICT $opt -o "$BIN/proto" test_protocol.c || fail "protocol compile $opt"
  "$BIN/proto" >/dev/null          || fail "protocol run $opt"
  echo "   protocol tests pass ($opt)"
done

# ------------------------------------------------- 3. cross-check vs pyca
echo "== [3] cross-check reference crypto vs pyca/cryptography =="
gcc $STRICT -O2 -o "$BIN/dump" dump_vectors.c || fail "dump_vectors compile"
"$BIN/dump" | python3 crosscheck.py           || fail "cross-check vs pyca"

# ------------------------------------------------- 4. header-only split
echo "== [4] header-only split (separate translation units) =="
gcc $STRICT -O2 -c impl_tu.c -o "$BIN/impl.o" || fail "impl TU compile"
gcc $STRICT -O2 -c user_tu.c -o "$BIN/user.o" || fail "user TU compile"
gcc "$BIN/impl.o" "$BIN/user.o" -o "$BIN/split" || fail "split link"
"$BIN/split" >/dev/null                       || fail "split run"
echo "   one-impl/many-users split links + runs"

# ------------------------------------------------- 5. C++ includability
echo "== [5] C++ includability =="
g++ $CXXSTRICT -c cpp_impl.cpp    -o "$BIN/cppimpl.o" || fail "impl compiles as C++"
g++ $CXXSTRICT -c cpp_include.cpp -o "$BIN/cppinc.o"  || fail "C++ header include"
g++ "$BIN/cppinc.o" "$BIN/impl.o" -o "$BIN/cpplink"   || fail "C++ links vs C impl"
"$BIN/cpplink" >/dev/null                             || fail "C++ run"
echo "   C++ TU links against C impl; impl also compiles as C++"

echo
green "ALL VERIFICATION STAGES PASSED"
