# Raw Data Format

Format version: 1

Each file is UTF-8 JSON Lines (JSONL) with one record per line. Each record is a JSON object with these required fields:

- `format_version` (int): file format version.
- `record_index` (uint64): monotonically increasing zero-based index.
- `venue` (string): lowercase venue name, e.g. `coinbase`.
- `connection_id` (uint64): 1 for initial connection, increments on reconnect.
- `receive_wall_ns` (int64): wall-clock receive timestamp in nanoseconds.
- `receive_monotonic_ns` (int64): monotonic receive timestamp in nanoseconds.
- `payload` (string): the raw WebSocket message as a JSON string value. The original bytes are preserved; quotes and control characters are escaped per JSON string rules.

Timestamp units: nanoseconds.

Venue encoding: string names matching `core::Venue` (currently `coinbase`, `kraken`).

Indexing rules: the first record_index is zero; subsequent records MUST increment by exactly one. Readers must validate contiguity and report noncontiguous indexes as an error.

Payload-preservation guarantee: the `payload` value, when parsed from the JSON string, reproduces the original message bytes exactly. The recorder must not parse and reserialize the payload as a JSON object before storing.

Corruption behavior: readers must report malformed JSON, missing fields, unknown format versions, noncontiguous indexes, or type mismatches as errors. Do not silently skip corrupted records.

Flush policy: the reference implementation flushes every record; implementations may buffer for performance but must accept the durability tradeoffs.

Why record before parsing: recording raw messages prior to parsing ensures every processed market event has a corresponding raw evidence file for debugging, replay, and audit.
