"""La désinstallation restaure les raccourcis avant de retirer leur exécutable."""
from contextlib import ExitStack, redirect_stdout
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import uninstall
import krusader_shortcut


class UninstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name)
        self.data = self.home / 'data'
        self.binary = self.home / '.local/bin/gad'
        self.binary.parent.mkdir(parents=True)
        self.binary.write_text('binary')
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        for target, name, value in [(uninstall, 'data_home', self.data),
                                    (uninstall, 'config_home', self.home / 'config'),
                                    (krusader_shortcut, 'data_home', self.data),
                                    (Path, 'home', self.home)]:
            self.stack.enter_context(patch.object(target, name, return_value=value))
        self.stack.enter_context(patch.object(uninstall, 'configure'))
        self.stack.enter_context(patch.object(uninstall, 'restore_launcher'))
        self.stack.enter_context(patch.object(uninstall.shutil, 'which', return_value=None))
        self.stack.enter_context(redirect_stdout(io.StringIO()))

    def test_restores_krusader_and_removes_konqueror_without_deleting_history(self):
        krusader_shortcut.configure(True, self.binary)
        history = self.data / 'gad/state.json'
        history.write_text('history')
        launcher = self.binary.with_name('gad-konqueror')
        launcher.write_text('launcher')
        for relative in ['applications/org.hizoka.gad.konqueror.desktop',
                         'konqueror/partsrcfiles/gad_konqueror.rc',
                         'gad/konqueror/plugins/test.so',
                         'gad/integration/krusader_shortcut.py']:
            path = self.data / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('installed')
        uninstall.uninstall()
        self.assertFalse(self.binary.exists())
        self.assertFalse(launcher.exists())
        self.assertNotIn('gad_paste', (self.data / 'krusader/useractions.xml').read_text())
        self.assertFalse((self.data / 'gad/konqueror').exists())
        self.assertFalse((self.data / 'gad/integration/krusader_shortcut.py').exists())
        self.assertEqual(history.read_text(), 'history')

    def test_refuses_binary_removal_when_shortcut_restoration_fails(self):
        with patch.object(uninstall, 'configure_krusader', side_effect=ValueError('invalid receipt')):
            with self.assertRaises(ValueError):
                uninstall.uninstall()
        self.assertTrue(self.binary.exists())
