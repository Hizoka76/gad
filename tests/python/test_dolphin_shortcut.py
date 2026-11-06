"""Vérifie les modifications XML réelles du raccourci, uniquement dans un répertoire temporaire."""
import os
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from xml.dom import minidom

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import dolphin_shortcut as shortcut


class DolphinShortcutTest(unittest.TestCase):
    def setUp(self):
        capture = patch("sys.stdout", new=io.StringIO())
        capture.start()
        self.addCleanup(capture.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.environment = patch.dict(os.environ, {"HOME": str(self.base), "XDG_DATA_HOME": str(self.base / "data")})
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.path = self.base / "data/kxmlgui5/dolphin/dolphinui.rc"

    def write(self, xml):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(xml, encoding="utf-8")

    def actions(self):
        return {node.getAttribute("name"): node for node in minidom.parse(str(self.path)).getElementsByTagName("Action")}

    def test_install_is_idempotent_and_preserves_unrelated_settings_and_backup(self):
        original = '<gui name="dolphin"><ActionProperties scheme="Default"><Action name="other" shortcut="Ctrl+X"/></ActionProperties></gui>'
        self.write(original)
        shortcut.configure()
        first = self.path.read_bytes()
        shortcut.configure()
        self.assertEqual(self.path.read_bytes(), first)
        self.assertEqual(self.actions()["other"].getAttribute("shortcut"), "Ctrl+X")
        self.assertEqual(self.actions()[shortcut.ACTION].getAttribute("shortcut"), shortcut.SHORTCUT)
        self.assertEqual(self.path.with_name(self.path.name + ".gad.bak").read_text(), original)

    def test_removal_preserves_user_reassigned_shortcut(self):
        self.write(f'<gui><ActionProperties><Action name="{shortcut.ACTION}" shortcut="Alt+V" icon="custom"/></ActionProperties></gui>')
        shortcut.configure(remove=True)
        action = self.actions()[shortcut.ACTION]
        self.assertEqual(action.getAttribute("shortcut"), "Alt+V")
        self.assertEqual(action.getAttribute("icon"), "custom")

    def test_removal_only_removes_owned_shortcut_and_preserves_other_attributes(self):
        self.write(f'<gui><ActionProperties><Action name="{shortcut.ACTION}" shortcut="{shortcut.SHORTCUT}" icon="custom"/><Action name="other" shortcut="Ctrl+O"/></ActionProperties></gui>')
        shortcut.configure(remove=True)
        self.assertFalse(self.actions()[shortcut.ACTION].hasAttribute("shortcut"))
        self.assertEqual(self.actions()[shortcut.ACTION].getAttribute("icon"), "custom")
        self.assertEqual(self.actions()["other"].getAttribute("shortcut"), "Ctrl+O")

    def test_unrelated_action_is_not_migrated(self):
        self.write('<gui><ActionProperties><Action name="unrelated" shortcut="Alt+G"/></ActionProperties></gui>')
        shortcut.configure()
        self.assertIn(shortcut.ACTION,self.actions())
        self.assertEqual(self.actions()["unrelated"].getAttribute("shortcut"),"Alt+G")

    def test_existing_kxmlgui6_location_is_used(self):
        self.path = self.base / "data/kxmlgui6/dolphin/dolphinui.rc"
        self.write('<gui name="dolphin"/>')
        shortcut.configure()
        self.assertIn(shortcut.ACTION, self.actions())
        self.assertFalse((self.base / "data/kxmlgui5").exists())

    def test_remove_missing_configuration_creates_nothing(self):
        shortcut.configure(remove=True)
        self.assertFalse((self.base / "data").exists())


    def test_ctrl_v_enable_and_disable_restore_only_shortcut_attributes(self):
        self.write('<gui><ActionProperties><Action name="edit_paste" shortcut="Alt+V" priority="0"/><Action name="other" shortcut="Ctrl+O"/></ActionProperties></gui>')
        shortcut.configure()
        before=self.path.read_bytes()
        shortcut.configure_paste(True)
        self.assertEqual(self.actions()[shortcut.CURRENT_ACTION].getAttribute("shortcut"),shortcut.PASTE_SHORTCUT)
        self.assertEqual(self.actions()[shortcut.NATIVE_ACTION].getAttribute("shortcut"),shortcut.NATIVE_SHORTCUT)
        self.assertEqual(self.actions()[shortcut.NATIVE_ACTION].getAttribute("priority"),"0")
        self.assertEqual(self.path.with_name(self.path.name+".gad-paste.bak").read_bytes(),before)
        shortcut.configure_paste(False)
        self.assertEqual(self.actions()[shortcut.ACTION].getAttribute("shortcut"),shortcut.SHORTCUT)
        self.assertEqual(self.actions()[shortcut.NATIVE_ACTION].getAttribute("shortcut"),"Alt+V")
        self.assertEqual(self.actions()["other"].getAttribute("shortcut"),"Ctrl+O")

    def test_ctrl_v_restores_absence_of_native_override(self):
        self.write('<gui><ActionProperties><Action name="edit_paste" priority="0"/></ActionProperties></gui>')
        shortcut.configure_paste(True);shortcut.configure_paste(False)
        self.assertFalse(self.actions()[shortcut.NATIVE_ACTION].hasAttribute("shortcut"))
        self.assertEqual(self.actions()[shortcut.NATIVE_ACTION].getAttribute("priority"),"0")

    def test_ctrl_v_enable_and_reinstall_are_idempotent(self):
        shortcut.configure_paste(True)
        original_receipt=shortcut.receipt_path().read_bytes()
        enabled=self.path.read_bytes()
        shortcut.configure_paste(True)
        self.assertEqual(self.path.read_bytes(),enabled)
        self.assertEqual(shortcut.receipt_path().read_bytes(),original_receipt)
        shortcut.configure()
        self.assertEqual(self.actions()[shortcut.CURRENT_ACTION].getAttribute("shortcut"),shortcut.PASTE_SHORTCUT)
        shortcut.configure_paste(False)
        self.assertEqual(self.actions()[shortcut.ACTION].getAttribute("shortcut"),shortcut.SHORTCUT)

    def test_ctrl_v_disable_preserves_user_changes_after_enable(self):
        shortcut.configure_paste(True)
        document=minidom.parse(str(self.path))
        for node in document.getElementsByTagName("Action"):
            if node.getAttribute("name")==shortcut.NATIVE_ACTION:node.setAttribute("shortcut","Meta+V")
            if node.getAttribute("name")==shortcut.ACTION:node.setAttribute("shortcut","Alt+G")
        self.path.write_bytes(document.toxml(encoding="UTF-8"))
        shortcut.configure_paste(False)
        self.assertEqual(self.actions()[shortcut.NATIVE_ACTION].getAttribute("shortcut"),"Meta+V")
        self.assertEqual(self.actions()[shortcut.ACTION].getAttribute("shortcut"),"Alt+G")

    def test_ctrl_v_uninstall_restores_native_and_removes_owned_gad_shortcut(self):
        self.write('<gui><ActionProperties><Action name="edit_paste" priority="0"/></ActionProperties></gui>')
        shortcut.configure_paste(True);shortcut.configure(remove=True)
        self.assertFalse(self.actions()[shortcut.NATIVE_ACTION].hasAttribute("shortcut"))
        self.assertNotIn(shortcut.ACTION,self.actions())

    def test_ctrl_v_conflict_preflight_changes_no_configuration(self):
        original='<gui><ActionProperties><Action name="custom" shortcut="Shift+Ins"/></ActionProperties></gui>'
        self.write(original)
        with self.assertRaises(ValueError):shortcut.configure_paste(True)
        self.assertEqual(self.path.read_text(),original)
        self.assertFalse(shortcut.receipt_path().exists())

    def test_ctrl_v_validates_all_xml_files_before_writing_any(self):
        self.write('<gui/>');other=self.base / "data/kxmlgui6/dolphin/dolphinui.rc"
        other.parent.mkdir(parents=True);other.write_text('<gui><ActionProperties><Action name="custom" shortcut="Ctrl+V"/></ActionProperties></gui>')
        before=(self.path.read_bytes(),other.read_bytes())
        with self.assertRaises(ValueError):shortcut.configure_paste(True)
        self.assertEqual((self.path.read_bytes(),other.read_bytes()),before)
        self.assertFalse(shortcut.receipt_path().exists())

    def test_ctrl_v_write_failure_rolls_back_already_written_xml(self):
        self.write('<gui/>');other=self.base / "data/kxmlgui6/dolphin/dolphinui.rc";other.parent.mkdir(parents=True);other.write_text('<gui/>')
        before=(self.path.read_bytes(),other.read_bytes());write=shortcut.atomic_write
        def fail_second(path,*args,**kwargs):
            if path==other:raise OSError("Écriture refusée")
            return write(path,*args,**kwargs)
        with patch.object(shortcut,"atomic_write",side_effect=fail_second):
            with self.assertRaises(OSError):shortcut.configure_paste(True)
        self.assertEqual((self.path.read_bytes(),other.read_bytes()),before)
        self.assertFalse(shortcut.receipt_path().exists())

    def test_ctrl_v_invalid_receipt_cannot_modify_xml_or_arbitrary_paths(self):
        import json
        self.write('<gui/>');before=self.path.read_bytes()
        outside=self.base / "outside.xml";outside.write_text('<gui/>')
        shortcut.receipt_path().parent.mkdir(parents=True)
        for value in [[],{"version":1,"files":{str(outside):{}}},{"version":1,"files":{str(self.path):{"bad":{}}}}]:
            shortcut.receipt_path().write_text(json.dumps(value))
            with self.assertRaises(ValueError):shortcut.configure_paste(False)
            self.assertEqual(self.path.read_bytes(),before);self.assertEqual(outside.read_text(),'<gui/>')

    def test_ctrl_v_private_receipt_and_other_schemes_are_preserved(self):
        self.write('<gui><ActionProperties scheme="Custom"><Action name="edit_paste" shortcut="Meta+V"/></ActionProperties></gui>')
        shortcut.configure_paste(True)
        self.assertEqual(shortcut.receipt_path().stat().st_mode & 0o777,0o600)
        document=minidom.parse(str(self.path));custom=[g for g in document.getElementsByTagName("ActionProperties") if g.getAttribute("scheme")=="Custom"][0]
        self.assertEqual(custom.getElementsByTagName("Action")[0].getAttribute("shortcut"),"Meta+V")

    def test_ctrl_v_disable_failure_keeps_receipt_and_restores_previous_xml(self):
        self.write('<gui/>');other=self.base / "data/kxmlgui6/dolphin/dolphinui.rc";other.parent.mkdir(parents=True);other.write_text('<gui/>')
        shortcut.configure_paste(True)
        before=(self.path.read_bytes(),other.read_bytes(),shortcut.receipt_path().read_bytes());write=shortcut.atomic_write
        def fail_second(path,*args,**kwargs):
            if path==other:raise OSError("Écriture refusée")
            return write(path,*args,**kwargs)
        with patch.object(shortcut,"atomic_write",side_effect=fail_second):
            with self.assertRaises(OSError):shortcut.configure_paste(False)
        self.assertEqual((self.path.read_bytes(),other.read_bytes(),shortcut.receipt_path().read_bytes()),before)
        shortcut.configure_paste(False)
        self.assertEqual(self.actions()[shortcut.ACTION].getAttribute("shortcut"),shortcut.SHORTCUT)

    def test_ctrl_v_backup_keeps_private_xml_permissions(self):
        self.write('<gui/>');self.path.chmod(0o600)
        shortcut.configure_paste(True)
        self.assertEqual(self.path.with_name(self.path.name+".gad-paste.bak").stat().st_mode & 0o777,0o600)
        self.assertEqual(self.path.stat().st_mode & 0o777,0o600)


    def test_ctrl_v_detects_modifier_order_aliases_and_multistroke_conflicts(self):
        for key in ("Ctrl+V, Ctrl+C","control+v","Shift+Insert"):
            original=f'<gui><ActionProperties><Action name="custom" shortcut="{key}"/></ActionProperties></gui>'
            self.write(original)
            with self.assertRaises(ValueError):shortcut.configure_paste(True)
            self.assertEqual(self.path.read_text(),original)


    def test_ctrl_v_migrates_old_selection_based_action_to_current_folder_action(self):
        import json
        self.write(f'<gui><ActionProperties><Action name="{shortcut.ACTION}" shortcut="{shortcut.PASTE_SHORTCUT}"/><Action name="{shortcut.NATIVE_ACTION}" shortcut="{shortcut.NATIVE_SHORTCUT}"/></ActionProperties></gui>')
        shortcut.receipt_path().parent.mkdir(parents=True)
        shortcut.receipt_path().write_text(json.dumps({"version":1,"files":{str(self.path):{
            shortcut.ACTION:{"present":True,"value":shortcut.SHORTCUT},shortcut.NATIVE_ACTION:{"present":False,"value":""}}}}))
        shortcut.configure_paste(True)
        self.assertEqual(self.actions()[shortcut.ACTION].getAttribute("shortcut"),"")
        self.assertEqual(self.actions()[shortcut.CURRENT_ACTION].getAttribute("shortcut"),shortcut.PASTE_SHORTCUT)
        self.assertEqual(shortcut.receipt()["version"],2)
        shortcut.configure_paste(False)
        self.assertEqual(self.actions()[shortcut.ACTION].getAttribute("shortcut"),shortcut.SHORTCUT)
        self.assertNotIn(shortcut.CURRENT_ACTION,self.actions())
        self.assertNotIn(shortcut.NATIVE_ACTION,self.actions())

    def test_ctrl_alt_v_remains_available_for_other_actions(self):
        self.write('<gui><ActionProperties><Action name="custom" shortcut="Ctrl+Alt+V"/></ActionProperties></gui>')
        shortcut.configure_paste(True)
        self.assertEqual(self.actions()["custom"].getAttribute("shortcut"),"Ctrl+Alt+V")
        self.assertEqual(self.actions()[shortcut.NATIVE_ACTION].getAttribute("shortcut"),"Shift+Ins")

    def test_old_native_fallback_migrates_without_losing_original(self):
        self.write('<gui><ActionProperties><Action name="edit_paste" shortcut="Alt+V"/></ActionProperties></gui>')
        with patch.object(shortcut,"NATIVE_SHORTCUT",shortcut.LEGACY_NATIVE_SHORTCUT):
            shortcut.configure_paste(True)
        shortcut.configure_paste(True)
        self.assertEqual(self.actions()[shortcut.NATIVE_ACTION].getAttribute("shortcut"),"Shift+Ins")
        shortcut.configure_paste(False)
        self.assertEqual(self.actions()[shortcut.NATIVE_ACTION].getAttribute("shortcut"),"Alt+V")

    def test_removed_shortcut_is_free_and_not_restored(self):
        self.write(f'<gui><ActionProperties><Action name="{shortcut.ACTION}" shortcut="{shortcut.REMOVED_SHORTCUT}"/><Action name="custom" shortcut="{shortcut.REMOVED_SHORTCUT}"/></ActionProperties></gui>')
        shortcut.configure_paste(True)
        self.assertEqual(self.actions()[shortcut.CURRENT_ACTION].getAttribute("shortcut"),"Ctrl+V")
        self.assertEqual(self.actions()["custom"].getAttribute("shortcut"),shortcut.REMOVED_SHORTCUT)
        shortcut.configure_paste(False)
        self.assertEqual(self.actions()[shortcut.ACTION].getAttribute("shortcut"),"")

    def test_current_action_loses_secondary_shortcut_on_reinstall(self):
        shortcut.configure_paste(True)
        document=minidom.parse(str(self.path))
        for node in document.getElementsByTagName("Action"):
            if node.getAttribute("name")==shortcut.CURRENT_ACTION:node.setAttribute("shortcut",shortcut.PREVIOUS_PASTE_SHORTCUT)
        self.path.write_bytes(document.toxml(encoding="UTF-8"))
        shortcut.configure()
        self.assertEqual(self.actions()[shortcut.CURRENT_ACTION].getAttribute("shortcut"),"Ctrl+V")
        shortcut.configure_paste(False)
        self.assertNotIn(shortcut.CURRENT_ACTION,self.actions())
