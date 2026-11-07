"""Le lanceur standard est protégé de façon réversible sans perdre les personnalisations."""
from pathlib import Path
import sys,tempfile,unittest,os
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))
import dolphin_integration as integration


class DolphinLauncherTests(unittest.TestCase):
    def prepare(self,root):
        system=root/'system/applications';system.mkdir(parents=True)
        (system/'org.kde.dolphin.desktop').write_text('[Desktop Entry]\nName=Dolphin\nExec="/usr/bin/dolphin" --select %u\nDBusActivatable=true\n')
        return patch.dict(os.environ,HOME=str(root),XDG_DATA_HOME=str(root/'data'),XDG_DATA_DIRS=str(root/'system'))

    def test_protect_preserves_arguments_and_restores_system_launcher(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            with self.prepare(root):
                binary=root/'my gad'
                integration.protect_launcher(binary)
                desktop=root/'data/applications/org.kde.dolphin.desktop'
                text=desktop.read_text();self.assertIn('"'+str(binary)+'" --dolphin --select %u',text);self.assertIn('DBusActivatable=false',text)
                integration.protect_launcher(binary);self.assertEqual(desktop.read_text(),text)
                integration.restore_launcher();self.assertFalse(desktop.exists())

    def test_original_customizations_are_restored(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            with self.prepare(root):
                desktop=root/'data/applications/org.kde.dolphin.desktop';desktop.parent.mkdir(parents=True)
                original='[Desktop Entry]\nName=Custom Dolphin\nExec=dolphin --new-window %U\n'
                desktop.write_text(original);integration.protect_launcher(root/'gad');integration.restore_launcher();self.assertEqual(desktop.read_text(),original)

    def test_later_user_changes_are_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            with self.prepare(root):
                integration.protect_launcher(root/'gad');desktop=root/'data/applications/org.kde.dolphin.desktop'
                desktop.write_text('user changed launcher')
                with self.assertRaises(RuntimeError):integration.protect_launcher(root/'gad')
                integration.restore_launcher();self.assertEqual(desktop.read_text(),'user changed launcher')
