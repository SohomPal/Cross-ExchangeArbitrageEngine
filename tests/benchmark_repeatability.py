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
