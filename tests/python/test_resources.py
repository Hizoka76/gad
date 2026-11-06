"""L’ajout de QM et de drapeaux est découvert sans modifier le manifeste."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import resources


class ResourceDiscoveryTest(unittest.TestCase):
    def test_compiled_catalog_without_ts_and_flag_are_embedded(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for directory in ("assets/icons", "assets/flags", "translations"):
                (root / directory).mkdir(parents=True)
            (root / "assets/icons/gad.qrc").write_text('<RCC><qresource prefix="/icons"><file alias="gad.png">gad.png</file></qresource></RCC>')
            for filename in ("assets/icons/gad.png", "assets/flags/de_DE.png", "translations/gad_de_DE.qm", "translations/ignored.txt"):
                (root / filename).touch()
            manifest = root / "generated.qrc"
            with patch.object(resources, "ROOT", root):
                resources.generate(manifest)
            groups = {group.get("prefix"): {item.get("alias"): item.text for item in group} for group in ET.parse(manifest).getroot()}
            self.assertEqual(groups["/i18n"], {"gad_de_DE.qm": str(root / "translations/gad_de_DE.qm")})
            self.assertIn("de_DE.png", groups["/flags"])
            self.assertEqual(groups["/icons"]["gad.png"], str(root / "assets/icons/gad.png"))
