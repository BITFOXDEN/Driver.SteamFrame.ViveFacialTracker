#!/usr/bin/env bash
# build_and_run.sh - builds and runs the vftstream test suite (Linux/g++).
#
# Usage: tests/build_and_run.sh [srcdir]   (default: ..)
# Exit code 0 iff everything passes.
set -euo pipefail
cd "$(dirname "$0")"
SRC=${1:-..}
CXX=${CXX:-g++}
# Production-like flags minus -mcpu=native (ARM-only; tests may run on x86).
FLAGS="-O3 -ffast-math -flto -std=c++17 -Wall -Wextra -Wno-unused-parameter"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

fail=0

echo "== building tests (g++: $($CXX --version | head -1)) =="
"$CXX" $FLAGS -o "$OUT/test_pipeline" test_pipeline.cpp "$SRC/src/pipeline.cpp" reference_pipeline.cpp || fail=1
"$CXX" $FLAGS -o "$OUT/test_topic"   test_topic.cpp                                    || fail=1
"$CXX" $FLAGS -o "$OUT/test_http"    test_http.cpp "$SRC/src/http_server.cpp" -pthread || fail=1
"$CXX" $FLAGS -o "$OUT/bench_pipeline" bench_pipeline.cpp "$SRC/src/pipeline.cpp" reference_pipeline.cpp || fail=1

[ "$fail" -eq 0 ] || { echo "BUILD FAILED"; exit 1; }

for t in test_pipeline test_topic test_http; do
    echo; echo "--- running $t ---"
    if ! "$OUT/$t"; then fail=1; fi
done

echo; echo "--- running bench_pipeline ---"
"$OUT/bench_pipeline"

echo
if [ "$fail" -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi
