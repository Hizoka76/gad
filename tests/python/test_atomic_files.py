"""Les publications générées remplacent seulement la cible et nettoient leurs temporaires."""
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from atomic_files import publish_bytes


class AtomicFilesTest(unittest.TestCase):
    def test_publication_replaces_final_symlink_without_following_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            unrelated = root/'keep';unrelated.write_bytes(b'keep')
            target = root/'guide';target.symlink_to(unrelated)
            publish_bytes(target, b'new')
            self.assertFalse(target.is_symlink())
            self.assertEqual(target.read_bytes(), b'new')
            self.assertEqual(unrelated.read_bytes(), b'keep')
            self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o644)

    def test_failed_publication_keeps_target_and_cleans_its_own_temporary(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root/'guide';target.write_bytes(b'original')
            with patch.object(Path, 'replace', side_effect=OSError('publication failed')):
                with self.assertRaises(OSError):
                    publish_bytes(target, b'new')
            self.assertEqual(target.read_bytes(), b'original')
            self.assertEqual(list(root.iterdir()), [target])

    def test_long_valid_target_name_does_not_lengthen_temporary_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary)/('a'*255)
            publish_bytes(target, b'data')
            self.assertEqual(target.read_bytes(), b'data')
