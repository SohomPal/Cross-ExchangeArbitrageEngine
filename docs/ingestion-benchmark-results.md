# Ingestion baseline results

Measured locally on 2026-09-12 using a Release build on Darwin-arm64, with one warm-up and five measured trials. This run overlapped development/build activity; use a quiet machine for controlled comparisons.

Source: `data/session-20260908T035554Z-11363.jsonl`  
SHA-256: `7ff9aaa2ac71899d9bd1c24bbcabac83fc92b7ec8ff0a5ffbf6545d18928fa03`

Every trial matched: 3,418 input/enqueued/written records, 3,217 successful book envelopes (one snapshot and 3,216 updates), 201 ignored messages, and zero failed outcomes. All four final indexes were 3,417; final sequence was 3,417.

Final book SHA-256: `9c929618e06b5e629ed2067ddbf692aa6d071156037f730265762f1fa9b64eb6`

| Trial | Snapshot (ms; one sample) | Update p50 (ms) | Update p95 (ms) | Update p99 (ms) | Update max (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 162.232 | 12.127 | 12.790 | 13.176 | 13.715 |
| 2 | 164.533 | 11.880 | 12.576 | 12.944 | 14.053 |
| 3 | 160.323 | 12.096 | 13.400 | 13.788 | 80.653 |
| 4 | 158.689 | 11.288 | 13.128 | 13.457 | 14.409 |
| 5 | 156.520 | 10.428 | 12.474 | 12.674 | 13.149 |

The historical baseline retained transactional full-book copies. These numbers establish a baseline; they do not isolate the cost of individual stages. The snapshot is reported separately so it cannot distort the normal update distribution.

See [methodology and commands](ingestion-benchmark.md). The generated JSON report includes all statistics and full book contents; it is intentionally not committed because this dataset produces a roughly 14 MiB report. Re-running produces fresh timing values and verifies the same counts and book hash.

## Sequential optimization measurements

Measured on 2026-09-12, Release/Darwin-arm64. Each row used the same source SHA-256 above, one warm-up, five measured trials, and unchanged bounds (4,096 messages / 64 MiB). Rows are sequential changes; profiling-on rows are comparison runs. No builds ran concurrently with these replays. Local scheduling, thermal state, and disk contention remain uncontrolled, so small differences are not evidence of a speedup.

All 60 measured trials matched the baseline final book contents/hash, outcomes, and recording progress. Each successful trial also verified every recorded payload after writer drain.

| Change | Median trial update p50 (µs) | Median trial update p99 (µs) |
| --- | ---: | ---: |
| Fresh baseline | 11604.125 | 13070.667 |
| Touched-price transactions | 29.208 | 246.167 |
| Message metadata and size buckets | 27.958 | 255.209 |
| Stage timers disabled | 27.083 | 240.000 |
| Stage timers enabled | 27.750 | 243.958 |
| Handler timing boundary | 26.875 | 238.333 |
| Reserve canonical vectors | 26.791 | 234.875 |
| Omit redundant owned raw copy | 26.458 | 238.208 |
| Skip L2 heartbeat decoding | 14.417 | 133.875 |
| Compact status level counts | 14.792 | 139.625 |
| Final (profiling accumulation fix) | 14.834 | 137.667 |
| Final with stage profiling | 15.583 | 138.334 |

Median trial p50 improved from 11.604 ms to 14.834 µs (about 782×). The first book-only change accounts for most of the gain. The final profiling-on/off p50 comparison changed by 5.0%; detailed timers remain off by default.

Final run, trial 1, incremental envelopes only:

| Changes | Samples | p50 (µs) | p95 (µs) | p99 (µs) |
| --- | ---: | ---: | ---: | ---: |
| 1 | 729 | 6.667 | 7.875 | 8.709 |
| 2-5 | 1122 | 11.792 | 16.958 | 18.084 |
| 6-20 | 1164 | 24.791 | 46.541 | 52.625 |
| 21-100 | 194 | 74.416 | 194.292 | 232.292 |
| 101+ | 7 | 252.209 | 269.625 | 269.625 |

Trial 1 processed 68,574 canonical levels, including the snapshot: 424,954 levels/s and 2,316.7 ns/level. Largest payload: 4,912,587 bytes; largest change count: 44,611; maximum-latency source record: 0 (snapshot). Throughput from the boundary-change row onward measures summed handler time and is not directly comparable to old loop throughput.

Full reports were written to `/tmp/ingestion-<row>.json` (earlier reports compressed as `.json.gz` to recover disk space). Final metadata and stage reports are `/tmp/ingestion-final.json` and `/tmp/ingestion-final-profile.json`. The compact machine-readable trial statistics are in [ingestion-optimization-results.json](ingestion-optimization-results.json).

Fixed-point character parsing, shared immutable envelopes, WebSocket buffer reuse, the three-thread recorder architecture, and terminal recorder failures were already implemented and were retained. DOM parser scratch reuse and allocation counting remain unimplemented; no custom allocator was added. Instruments could not run because `xctrace` is unavailable. See the [methodology](ingestion-benchmark.md) for the profiling command and precise timing boundaries.
