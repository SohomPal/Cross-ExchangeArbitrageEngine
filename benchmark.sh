#!/usr/bin/env bash
set -euo pipefail
input=${1:-data/session-20260908T035554Z-11363.jsonl}
output=${2:-benchmark-results-$(date -u +%Y%m%dT%H%M%SZ)-$$.json}
profile=${3:-off}
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/e2e_latency_benchmark \
  --input "$input" --trials 5 --warmup-trials 1 \
  --profile-stages "$profile" --output "$output"
ctest --test-dir build --output-on-failure
