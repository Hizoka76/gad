"""Vérifier le nettoyage sans données réelles et sans suivre les liens symboliques."""
from contextlib import redirect_stdout
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import clean


class CleanTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'project'
        self.root.mkdir()
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)

    def file(self, relative, content='generated'):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def track(self, relative):
        subprocess.run(['git', '-C', str(self.root), 'add', '-f', relative], check=True)

    def run_clean(self, apply=False):
        with redirect_stdout(io.StringIO()):
            return clean.clean(self.root, apply)

    def test_preview_does_not_remove_anything(self):
        path = self.file('target/debug/gad')
        self.assertIn(path.parent, self.run_clean())
        self.assertTrue(path.exists())

    def test_apply_removes_builds_caches_and_debian_outputs(self):
        names = ['target/debug/gad', 'target/packages-source/archive.tar.xz',
                 'scripts/__pycache__/module.pyc', '.pytest_cache/state',
                 'debian/gad/usr/bin/gad', 'debian/cargo-home/cache',
                 'debian/.debhelper/state', 'debian/gad.substvars',
                 'debian/files', 'gad_0.1_amd64.deb', 'editor.kate-swp',
                 'gad_0.1_source.changes', 'gad_0.1.dsc', 'gad_0.1.orig.tar.gz',
                 'gad_0.1.debian.tar.xz', 'gad_0.1_source.buildinfo']
        paths = [self.file(name) for name in names]
        self.run_clean(True)
        self.assertTrue(all(not path.exists() for path in paths))
        self.assertEqual(self.run_clean(True), [])

    def test_preserves_releases_sources_and_notes(self):
        names = ['target/releases/v0.1.0/gad.deb', 'target/releases/cache.pyc',
                 'src/main.rs', 'TOTO.md', 'private.txt', 'docs/html/guide.html']
        paths = [self.file(name) for name in names]
        self.run_clean(True)
        self.assertTrue(all(path.exists() for path in paths))

    def test_preserves_tracked_files_and_their_directories(self):
        names = ['target/special/keep', 'scripts/__pycache__/keep.pyc', 'tracked.deb']
        for name in names:
            self.file(name)
            self.track(name)
        self.run_clean(True)
        self.assertTrue(all((self.root / name).exists() for name in names))

    def test_target_symlink_is_unlinked_without_touching_external_data(self):
        outside = Path(self.temporary.name) / 'external'
        outside.mkdir()
        (outside / 'keep').write_text('private')
        (self.root / 'target').symlink_to(outside, target_is_directory=True)
        self.run_clean(True)
        self.assertFalse((self.root / 'target').is_symlink())
        self.assertEqual((outside / 'keep').read_text(), 'private')

    def test_nested_symlinks_never_delete_their_external_targets(self):
        outside = Path(self.temporary.name) / 'external'
        outside.mkdir()
        (outside / 'keep.pyc').write_text('private')
        (self.root / '__pycache__').symlink_to(outside, target_is_directory=True)
        (self.root / 'other').symlink_to(outside, target_is_directory=True)
        self.run_clean(True)
        self.assertFalse((self.root / '__pycache__').is_symlink())
        self.assertTrue((self.root / 'other').is_symlink())
        self.assertTrue((outside / 'keep.pyc').exists())

    def test_fails_closed_outside_git(self):
        outside = Path(self.temporary.name) / 'not-a-repository'
        outside.mkdir()
        path = outside / 'cache.pyc'
        path.write_text('keep')
        with self.assertRaises(subprocess.CalledProcessError):
            clean.clean(outside, True)
        self.assertTrue(path.exists())
