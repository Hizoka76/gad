"""Vérifier l’installation système isolée et l’exclusion des données privées des sources."""
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import install_system
import package


class PackagingTests(unittest.TestCase):
    def test_system_install_only_writes_destination(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / 'gad'
            binary.write_bytes(b'example executable')
            home = root / 'home'
            home.mkdir()
            with patch.dict(os.environ, HOME=str(home)), patch.object(install_system, 'install_docs') as docs:
                install_system.install(root / 'stage', binary)
            self.assertEqual(list(home.iterdir()), [])
            self.assertEqual(len(list((root / 'stage/usr/share/icons/hicolor').glob('*/apps/*.png'))), 9)
            self.assertEqual((root / 'stage/usr/bin/gad').read_bytes(), binary.read_bytes())
            self.assertEqual((root / 'stage/usr/bin/gad').stat().st_mode & 0o777, 0o755)
            launcher = (root / 'stage/usr/share/applications/org.hizoka.gad.desktop').read_text()
            self.assertIn('Exec=gad %u', launcher)
            self.assertNotIn(str(root), launcher)
            docs.assert_called_once_with(root / 'stage/usr/share/gad/docs')

    def test_source_archive_omits_private_and_build_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project = root / 'project'
            project.mkdir()
            (project / 'Cargo.toml').write_text('[package]\nversion="0.1.0"\n')
            for name in ('Cargo.lock', 'build.rs', 'Makefile', 'build.sh'):
                (project / name).write_text('fixture')
            for name in ('src', 'native', 'scripts', 'tests', 'translations', 'assets', 'docs', 'packaging', 'debian', 'target'):
                (project / name).mkdir()
            (project / 'target/private').write_text('must stay private')
            (project / 'TOTO.md').write_text('private notes')
            def vendor(*args, **kwargs):
                (kwargs['cwd'] / 'vendor').mkdir()
                return type('Result', (), {'stdout': '[source.vendored-sources]\ndirectory="vendor"\n'})()
            with patch.object(package, 'PROJECT', project), patch.object(package.subprocess, 'run', side_effect=vendor), patch.object(package.subprocess, 'check_output', return_value='0'):
                source = package.prepare(root / 'output')
            self.assertTrue((source / 'debian').is_dir())
            with tarfile.open(root / 'output/gad_0.1.0.orig.tar.xz') as archive:
                names = archive.getnames()
                self.assertFalse(any('/target' in name or '/debian' in name or 'TOTO' in name for name in names))
                self.assertTrue(any('/vendor' in name for name in names))
                self.assertTrue(all(member.mtime == 0 for member in archive.getmembers()))
            with patch.object(package, 'PROJECT', project):
                with self.assertRaises(SystemExit):
                    package.prepare(root / 'output')
