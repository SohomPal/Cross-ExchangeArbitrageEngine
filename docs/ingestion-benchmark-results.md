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

The implementation retains transactional full-book copies. These numbers establish a baseline; they do not isolate the cost of individual stages. The snapshot is reported separately so it cannot distort the normal update distribution.

See [methodology and commands](ingestion-benchmark.md). The generated JSON report includes all statistics and full book contents; it is intentionally not committed because this dataset produces a roughly 14 MiB report. Re-running produces fresh timing values and verifies the same counts and book hash.
