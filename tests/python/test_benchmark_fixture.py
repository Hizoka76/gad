"""Les mesures annoncent le nombre réel de fichiers, même dans une arborescence déséquilibrée."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from benchmark_rename import fixture


class BenchmarkFixtureTest(unittest.TestCase):
    def test_exact_file_count_and_empty_folders(self):
        for files, folders in [(8, 3), (2, 4), (9, 1)]:
            with self.subTest(files=files, folders=folders), tempfile.TemporaryDirectory() as tmp:
                source = fixture(Path(tmp), "source", files, folders)
                self.assertEqual(sum(path.is_file() for path in source.rglob("*")), files)
                self.assertEqual(sum(path.is_dir() for path in source.iterdir()), folders)
                self.assertTrue(all(path.read_bytes() == b"benchmark" for path in source.rglob("*") if path.is_file()))
