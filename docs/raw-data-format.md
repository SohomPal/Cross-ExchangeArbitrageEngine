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

Why enqueue before parsing: every processed message must first be accepted into the bounded raw-recording FIFO. The writer serializes and flushes accepted messages in order. Queue acceptance is not a disk-write acknowledgement: a terminal writer failure can leave an unpersisted processed tail. Live status reports received, enqueued, written, and processed positions separately, including the exact unpersisted processed range. On clean shutdown the queue drains and all four positions agree. See [architecture](architecture.md) for thread ownership and shutdown behavior.

Recovery appends to the same recording file. `connection_id` increments after each successful session's subscriptions are sent. Sequence baselines are per connection; never compare envelope sequences across these boundaries. All sequenced channels participate in continuity checks within a connection. Raw messages that reveal an integrity failure are recorded before rejection. Socket failures and health timer events are not raw messages and are not encoded as synthetic exchange payloads.
