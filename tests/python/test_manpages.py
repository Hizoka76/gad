"""Les pages générées et leur installation respectent les conventions de man(1)."""
from pathlib import Path
import sys
import tempfile
import unittest
import json
import shutil
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import generate_manpages
import install_manpages


class ManpageTests(unittest.TestCase):
    def test_generated_pages_are_current_and_cover_public_options(self):
        for name, options in [('gad', generate_manpages.GAD_OPTIONS)]:
            for language in generate_manpages.LANGUAGES:
                rendered = generate_manpages.render(name, language)
                path = generate_manpages.ROOT / 'packaging' / generate_manpages.filename(name, language)
                self.assertEqual(path.read_text(), rendered)
                for flag, _, _ in options:
                    self.assertIn(generate_manpages.escaped(flag), rendered)

    def test_install_uses_language_directories_and_preserves_other_manuals(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            untouched = root / 'unrelated'
            untouched.write_text('keep')
            install_manpages.install(root)
            self.assertEqual(untouched.read_text(), 'keep')
            self.assertTrue((root / 'man1/gad.1').is_file())
            self.assertTrue((root / 'fr/man1/gad.1').is_file())
            install_manpages.install(root)
            self.assertEqual(len(list(root.rglob('*.1'))), 6)
            for language in generate_manpages.LANGUAGES:
                folder = root / (language if language != 'en' else '') / 'man1'
                self.assertTrue((folder / 'gad.1').is_file())

    def test_all_application_languages_and_debian_pages_are_covered(self):
        locales = {path.stem.removeprefix('gad_').split('_')[0] for path in (generate_manpages.ROOT / 'translations').glob('gad_*.ts')} | {'en'}
        self.assertEqual(set(generate_manpages.LANGUAGES), locales)
        installed = (generate_manpages.ROOT / 'debian/gad.manpages').read_text().splitlines()
        expected = {'packaging/' + generate_manpages.filename(name, language) for name in ('gad',) for language in generate_manpages.LANGUAGES}
        self.assertEqual(set(installed), expected)

    def test_missing_translation_fails_instead_of_falling_back_to_english(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shutil.copy2(generate_manpages.ROOT / 'Cargo.toml', root / 'Cargo.toml')
            catalog = json.loads((generate_manpages.ROOT / 'packaging/manpages/es.json').read_text())
            del catalog['texts'][next(iter(catalog['texts']))]
            folder = root / 'packaging/manpages'
            folder.mkdir(parents=True)
            (folder / 'es.json').write_text(json.dumps(catalog))
            with patch.object(generate_manpages, 'ROOT', root):
                with self.assertRaises(KeyError):
                    generate_manpages.render('gad', 'es')
