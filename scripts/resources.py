#!/usr/bin/env python3
"""Génère le manifeste Qt depuis les catalogues compilés et leurs drapeaux."""
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent


def generate(destination):
    """Les noms de fichiers servent d’alias ; aucun catalogue n’est énuméré à la main."""
    root = ET.parse(ROOT / "assets/icons/gad.qrc").getroot()
    for item in root.findall(".//file"):
        item.text = str((ROOT / "assets/icons" / item.text).resolve())
    for prefix, files in (("/flags", sorted((ROOT / "assets/flags").glob("*.png"))),
                          ("/i18n", sorted((ROOT / "translations").glob("gad_*.qm")))):
        group = ET.SubElement(root, "qresource", prefix=prefix)
        for path in files:
            ET.SubElement(group, "file", alias=path.name).text = str(path.resolve())
    ET.indent(root)
    ET.ElementTree(root).write(destination, encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    generate(Path(sys.argv[1]))
