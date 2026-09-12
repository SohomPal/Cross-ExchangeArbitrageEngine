cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/e2e_latency_benchmark \
  --input data/session-20260908T035554Z-11363.jsonl \
  --trials 5 --warmup-trials 1 --output benchmark-results.json
ctest --test-dir build --output-on-failure