"""Contrôles du catalogue embarqué sans dépendance aux outils Linguist."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import translations


class TranslationsTest(unittest.TestCase):
    def test_all_marked_texts_are_translated_and_parameters_preserved(self):
        for catalog in sorted((translations.ROOT / "translations").glob("gad_*.ts")):
            locale = catalog.stem.removeprefix("gad_")
            self.assertEqual(translations.validate(catalog, expected=translations.sources(), language=locale), [])

    def test_compiled_catalog_matches_sources(self):
        for catalog in sorted((translations.ROOT / "translations").glob("gad_*.ts")):
            locale = catalog.stem.removeprefix("gad_")
            stamp = catalog.with_suffix(".sha256.json")
            self.assertEqual(json.loads(stamp.read_text()), translations.fingerprint(catalog))

    def test_extraction_ignores_comments_and_unmarked_protocol(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "native").mkdir()
            (root / "native/example.cpp").write_text('// GAD_TR("comment")\n/* GAD_TR("comment too") */\nGAD_TR("Texte %1")\n"sftp://host"; GAD_TR("Autre")\n"waiting";\n')
            self.assertEqual(set(translations.sources(root)), {"Texte %1", "Autre"})

    def test_missing_text_is_reported(self):
        errors = translations.validate(expected={"Texte non traduit"})
        self.assertTrue(any("Texte non traduit" in error for error in errors))

    def test_changed_placeholder_and_link_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "invalid.ts"
            path.write_text('<TS language="fr_FR" sourcelanguage="en_US"><context><name>Gad</name><message><source>Go %1 &lt;a href="gad://show"&gt;</source><translation>Go %2 &lt;a href="gad://other"&gt;</translation></message></context></TS>')
            errors = translations.validate(path)
            self.assertTrue(any("Paramètres" in error for error in errors))
            self.assertTrue(any("Lien" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
