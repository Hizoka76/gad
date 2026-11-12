"""La configuration de Krusader conserve les actions utilisateur et se restaure sans écrasement."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
from unittest.mock import patch
from xml.dom import minidom
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import krusader_shortcut as integration


class KrusaderShortcutTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.data = Path(self.tmp.name)
        self.mock = patch.object(integration, 'data_home', return_value=self.data)
        self.mock.start();self.addCleanup(self.mock.stop)
        self.binary = self.data / "bin/Gad's executable"
        self.user = self.data / 'krusader/useractions.xml'
        self.rc = self.data / 'kxmlgui5/krusader/krusaderui.rc'

    def enable(self):
        integration.configure(True, self.binary)

    def test_active_panel_url_and_safe_executable(self):
        self.enable()
        doc = minidom.parse(str(self.user))
        command = doc.getElementsByTagName('command')[0]
        self.assertEqual(command.getAttribute('accept'), 'url')
        self.assertTrue(command.firstChild.data.endswith(' --paste %aPath%'))
        import shlex
        self.assertEqual(shlex.split(command.firstChild.data)[0], str(self.binary))
        self.assertNotIn('F5', self.rc.read_text())
        self.assertNotIn('F6', self.rc.read_text())
        self.assertIn('name="edit_paste" shortcut=""', self.rc.read_text())

    def test_enable_is_idempotent_and_disable_restores(self):
        self.user.parent.mkdir(parents=True)
        self.user.write_text('<KrusaderUserActions><!--keep--><action name="other"><title>Other</title></action></KrusaderUserActions>')
        self.rc.parent.mkdir(parents=True)
        self.rc.write_text('<gui name="krusader"><ActionProperties><Action name="std_paste" shortcut="Ctrl+V; Alt+P"/><Action name="unrelated" shortcut="F8"/></ActionProperties></gui>')
        self.enable();first=self.user.read_bytes();self.enable()
        self.assertEqual(first,self.user.read_bytes())
        self.assertIn('shortcut="Alt+P"',self.rc.read_text())
        integration.configure(False,self.binary)
        self.assertNotIn('gad_paste',self.user.read_text())
        self.assertIn('<!--keep-->',self.user.read_text())
        self.assertIn('Ctrl+V; Alt+P',self.rc.read_text())
        self.assertIn('unrelated',self.rc.read_text())

    def test_user_changes_are_preserved(self):
        self.enable();self.rc.write_text(self.rc.read_text().replace('shortcut="Ctrl+V"','shortcut="Alt+G"'))
        integration.configure(False,self.binary)
        self.assertIn('Alt+G',self.rc.read_text())

    def test_conflicting_shortcut_makes_no_changes(self):
        self.user.parent.mkdir(parents=True)
        self.user.write_text('<KrusaderUserActions><action name="other"><defaultshortcut>Ctrl+V</defaultshortcut></action></KrusaderUserActions>')
        before=self.user.read_bytes()
        with self.assertRaises(ValueError):self.enable()
        self.assertEqual(before,self.user.read_bytes());self.assertFalse(self.rc.exists())
        self.assertFalse(integration.receipt_path().exists())

    def test_invalid_xml_makes_no_changes(self):
        self.rc.parent.mkdir(parents=True);self.rc.write_text('<broken')
        with self.assertRaises(Exception):self.enable()
        self.assertFalse(self.user.exists());self.assertFalse(integration.receipt_path().exists())

    def test_both_xmlgui_versions_are_configured(self):
        second=self.data/'kxmlgui6/krusader/krusaderui.rc'
        for p in (self.rc,second):p.parent.mkdir(parents=True);p.write_text('<gui/>')
        self.enable()
        for p in (self.rc,second):self.assertIn('gad_paste',p.read_text())

    def test_receipt_cannot_write_outside_configuration(self):
        self.enable();receipt=integration.receipt_path();state=json.loads(receipt.read_text());state['files']['/tmp/not-gad']={};receipt.write_text(json.dumps(state))
        with self.assertRaises(ValueError):integration.configure(False,self.binary)

    def test_enable_recovers_after_receipt_or_partial_xml_publication(self):
        for crash_target in (self.user, self.rc):
            with self.subTest(crash_target=crash_target):
                self.user.parent.mkdir(parents=True, exist_ok=True)
                self.rc.parent.mkdir(parents=True, exist_ok=True)
                self.user.write_text('<KrusaderUserActions><action name="other"><title>Keep</title></action></KrusaderUserActions>')
                self.rc.write_text('<gui name="krusader"><ActionProperties><Action name="std_paste" shortcut="Ctrl+V; Alt+P"/></ActionProperties></gui>')
                writer = integration.atomic_write
                def interrupted(path, *args, **kwargs):
                    if path == crash_target:
                        raise SystemExit('simulated interruption')
                    return writer(path, *args, **kwargs)
                with patch.object(integration, 'atomic_write', side_effect=interrupted):
                    with self.assertRaises(SystemExit):
                        self.enable()
                self.enable()
                self.assertIn('shortcut="Alt+P"', self.rc.read_text())
                integration.configure(False, self.binary)
                self.assertIn('shortcut="Ctrl+V; Alt+P"', self.rc.read_text())
                self.assertIn('name="other"', self.user.read_text())
                self.assertNotIn('gad_paste', self.user.read_text())

    def test_reenable_refuses_unknown_changes_without_writing_any_file(self):
        self.enable()
        self.rc.write_text(self.rc.read_text().replace('shortcut="Ctrl+V"', 'shortcut="Alt+G"'))
        paths = (self.user, self.rc, integration.receipt_path())
        before = [path.read_bytes() for path in paths]
        with self.assertRaises(ValueError):
            self.enable()
        self.assertEqual([path.read_bytes() for path in paths], before)
