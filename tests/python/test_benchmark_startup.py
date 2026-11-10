"""Le benchmark de reprise ne prépare aucune opération de fichier exécutable."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from benchmark_startup import prepare
from journal_reader import read_journal


class StartupBenchmarkTest(unittest.TestCase):
    def test_fixture_has_only_completed_private_entries_and_valid_records(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            size = prepare(root, files=3, records=2)
            path = root / "data/gad/state.json"
            self.assertEqual(size, path.with_suffix(".json.wal").stat().st_size)
            state = read_journal(path)
            self.assertTrue(state["keep_history"])
            self.assertEqual(len(state["jobs"]), 1)
            job = state["jobs"][0]
            self.assertEqual(job["state"], "done")
            self.assertEqual(len(job["entries"]), 3)
            for entry in job["entries"]:
                self.assertEqual(entry["state"], "done")
                self.assertFalse(entry.get("undo"))
                for key in ("source", "destination"):
                    value = Path(entry[key])
                    self.assertTrue(value.is_relative_to(root))
                    self.assertFalse(value.exists())

    def test_snapshot_only_has_no_records(self):
        for kind in ("legacy", "delta"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                root=Path(temporary)
                self.assertEqual(prepare(root,2,0,kind),0)
                state=read_journal(root / "data/gad/state.json")
                self.assertEqual(len(state["jobs"][0]["entries"]),2)
                self.assertEqual(state["jobs"][0]["error"],"")

    def test_delta_records_preserve_utf8_offsets_and_final_state(self):
        for count in (1,3,10):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as temporary:
                root=Path(temporary) / "é😀"
                prepare(root,3,count,"delta")
                state=read_journal(root / "data/gad/state.json")
                self.assertEqual(state["jobs"][0]["error"],f"GAD-BENCH-{count-1:06d}")
                self.assertEqual(len(state["jobs"][0]["entries"]),3)
                self.assertTrue(all(entry["state"]=="done" for entry in state["jobs"][0]["entries"]))

    def test_custom_order_survives_delta_headers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            prepare(root,3,4,"delta",True)
            state=read_journal(root / "data/gad/state.json")
            self.assertEqual(state["row_order"],[{"job":1,"index":index} for index in range(3)])
