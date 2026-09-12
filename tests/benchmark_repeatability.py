"""Golden deterministic correctness checks; deliberately no timing thresholds."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

binary, fixture, live = sys.argv[1:]
contents = {
    "asks": [[6214450, 100000000], [6214500, 400000000]],
    "bids": [[6214327, 700000000], [6214200, 200000000]],
    "book_sequence": 1,
    "state": 1,
}
expected_hash = hashlib.sha256(
    json.dumps(contents, sort_keys=True, separators=(",", ":")).encode()
).hexdigest()
with tempfile.TemporaryDirectory(prefix="benchmark-test-") as directory:
    output = pathlib.Path(directory) / "report.json"
    subprocess.run([binary, "--input", fixture, "--trials", "3", "--warmup-trials", "1",
                    "--output", str(output)], check=True, timeout=30)
    report = json.loads(output.read_text())
    assert report["source_sha256"] == hashlib.sha256(pathlib.Path(fixture).read_bytes()).hexdigest()
    assert len(report["trials"]) == 3
    for trial in report["trials"]:
        assert trial["outcomes"] == {
            "BookUpdated": 4, "Ignored": 3, "Duplicate": 1, "OutOfOrder": 1,
            "ParseError": 1, "SequenceGap": 1, "ApplyError": 1, "RecordingRejected": 0,
        }
        assert trial["final_book_contents"] == contents
        assert trial["final_book_hash"] == expected_hash
        assert trial["final_sequence"] == 2
        assert trial["final_best_bid"] == 6214327
        assert trial["final_best_ask"] == 6214450
        for key in ["input_messages", "written_records", "enqueued_records"]:
            assert trial[key] == 12
        for key in ["last_received_index", "last_enqueued_index", "last_written_index", "last_processed_index"]:
            assert trial[key] == 11
        assert trial["snapshot_latency"]["samples"] == 2
        assert trial["update_latency"]["samples"] == 2
        assert trial["all_book_latency"]["samples"] == 4
    # Verify metadata and nearest-rank bucket statistics against actual samples.
    for trial in report["trials"]:
        records = trial["records"]
        assert [r["record_index"] for r in records] == list(range(12))
        successful = [r for r in records if r["latency_ns"] is not None]
        assert len(successful) == trial["book_messages"]
        assert sum(r["number_of_changes"] for r in successful) == trial["levels_processed"]
        assert max(r["payload_bytes"] for r in records) == trial["largest_payload_bytes"]
        assert max(r["number_of_changes"] for r in records) == trial["largest_change_count"]
        slowest = max(successful, key=lambda r: r["latency_ns"])
        assert trial["maximum_latency_record_index"] == slowest["record_index"]
        assert trial["handler_elapsed_ns"] >= sum(r["latency_ns"] for r in successful)
        assert all("stages" not in r for r in records)
        bounds = {"0": (0, 0), "1": (1, 1), "2-5": (2, 5), "6-20": (6, 20),
                  "21-100": (21, 100), "101+": (101, float("inf"))}
        import math
        for label, (low, high) in bounds.items():
            values = sorted(r["latency_ns"] for r in successful
                            if r["snapshot_or_update"] == "update"
                            and low <= r["number_of_changes"] <= high)
            stats = trial["update_size_latency"][label]
            assert stats["samples"] == len(values)
            for percentile in (50, 95, 99):
                assert stats[f"p{percentile}_ns"] == (
                    values[math.ceil(percentile / 100 * len(values)) - 1] if values else 0)
    profiled_output = pathlib.Path(directory) / "profiled.json"
    subprocess.run([binary, "--input", fixture, "--trials", "2", "--warmup-trials", "1",
                    "--profile-stages", "on", "--output", str(profiled_output)],
                   check=True, timeout=30)
    profiled = json.loads(profiled_output.read_text())
    assert profiled["profile_stages"] is True
    for trial in profiled["trials"]:
        assert trial["final_book_hash"] == expected_hash
        assert trial["outcomes"] == report["trials"][0]["outcomes"]
        assert len(trial["stage_latency"]) == 8
        for name, stats in trial["stage_latency"].items():
            assert stats["samples"] == 12
            assert all(0 <= r["stages"][name] <= trial["handler_elapsed_ns"]
                       for r in trial["records"])
    # Overflow must fail, even with malformed input: there is no unbounded fallback.
    failed = subprocess.run([binary, "--input", fixture, "--trials", "1", "--warmup-trials", "1",
                             "--queue-bytes", "1", "--output", str(pathlib.Path(directory) / "overflow.json")],
                            capture_output=True, text=True, timeout=30)
    assert failed.returncode != 0
    assert "RAW_RECORDING_QUEUE_FULL" in failed.stderr

    preflight_path = pathlib.Path(directory) / "preflight.jsonl"
    preflight = subprocess.run([live, "--record", str(preflight_path),
                                "--minimum-free-disk-bytes", str(2**64 - 1)],
                               capture_output=True, text=True, timeout=10)
    assert preflight.returncode != 0
    assert "Insufficient disk space" in preflight.stdout
    assert "runtime=StoppedRecordingError" in preflight.stdout
    assert "received=0" in preflight.stdout
    assert not preflight_path.exists()
