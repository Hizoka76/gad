"""Le lecteur de diagnostic suit les mêmes frontières d’enregistrement que Rust."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from journal_reader import read_journal


class JournalReaderTest(unittest.TestCase):
    def test_replays_changed_jobs_and_ignores_uncommitted_tail(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "state.json"
            path.write_text(json.dumps({"jobs": [{"id": 1}, {"id": 2}], "paused": False}))
            payload = json.dumps({"sequence": 1, "header": {"paused": True},
                                  "jobs": [{"id": 2, "state": "done"}], "ids": [2]}).encode()
            line = hashlib.md5(payload).hexdigest().encode() + b" " + payload + b"\n"
            path.with_suffix(".json.wal").write_bytes(line + b"incomplete")
            self.assertEqual(read_journal(path), {"paused": True, "jobs": [{"id": 2, "state": "done"}]})
            path.with_suffix(".json.wal").write_bytes(b"damaged\n")
            with self.assertRaises(ValueError):
                read_journal(path)

    def test_compacted_snapshot_skips_old_records_and_detects_gaps(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "state.json"
            path.write_text(json.dumps({"_gad_journal_seq": 1, "jobs": [], "paused": True}))
            def record(sequence):
                payload = json.dumps({"sequence": sequence, "header": {"paused": False}, "jobs": [], "ids": []}).encode()
                return hashlib.md5(payload).hexdigest().encode() + b" " + payload + b"\n"
            path.with_suffix(".json.wal").write_bytes(record(1))
            self.assertEqual(read_journal(path), {"paused": True, "jobs": []})
            path.with_suffix(".json.wal").write_bytes(record(3))
            with self.assertRaises(ValueError):
                read_journal(path)

    def test_v2_patches_keep_exact_raw_json_and_unicode_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "state.json"
            path.write_text(json.dumps({"jobs": [], "paused": False}))
            old = '{ "id":1, "name":"é😀", "state":"running" }'.encode()
            new = old.replace(b'running', b'done')
            first = b'{"sequence":1,"header":{"paused":false},"ids":[1],"jobs":null,"format":2,"full_jobs":[' + old + b'],"patches":[]}'
            patch = {"id": 1, "base": hashlib.md5(old).hexdigest(), "result": hashlib.md5(new).hexdigest(),
                     "offset": old.index(b'running'), "remove": len(b'running'), "insert": "done"}
            second = json.dumps({"sequence": 2, "header": {"paused": False}, "ids": [1],
                                 "jobs": None, "format": 2, "full_jobs": [], "patches": [patch]}).encode()
            def line(payload):
                return hashlib.md5(payload).hexdigest().encode() + b" " + payload + b"\n"
            path.with_suffix(".json.wal").write_bytes(line(first) + line(second))
            self.assertEqual(read_journal(path)["jobs"], [json.loads(new)])
            patch["base"] = "invalid"
            second = json.dumps({"sequence": 2, "header": {}, "ids": [1], "jobs": None, "format": 2,
                                 "full_jobs": [], "patches": [patch]}).encode()
            path.with_suffix(".json.wal").write_bytes(line(first) + line(second))
            with self.assertRaises(ValueError):
                read_journal(path)

    def test_large_reordered_history_preserves_order_and_removes_absent_jobs(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "state.json"
            jobs = [{"id": index, "state": "done"} for index in range(2048)]
            path.write_text(json.dumps({"jobs": jobs, "paused": False}))
            ids = list(range(2046, -1, -2))
            payload = json.dumps({"sequence": 1, "header": {"paused": True},
                                  "jobs": [{"id": 0, "state": "cancelled"}], "ids": ids}).encode()
            path.with_suffix(".json.wal").write_bytes(hashlib.md5(payload).hexdigest().encode() + b" " + payload + b"\n")
            state = read_journal(path)
            self.assertEqual([job["id"] for job in state["jobs"]], ids)
            self.assertEqual(state["jobs"][-1]["state"], "cancelled")
            self.assertTrue(state["paused"])
