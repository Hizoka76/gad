#!/usr/bin/env python3
"""Synchronise les textes Qt marqués, vérifie les paramètres et compile le catalogue Linguist."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
CATALOG = ROOT / "translations/gad_fr_FR.ts"


def sources(root=ROOT):
    """Les seuls littéraux explicitement marqués sont présentés au traducteur, jamais les clés JSON."""
    result = {}
    for path in sorted((root / "native").glob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        text = path.read_text()
        for match in re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|GAD_TR\(("(?:\\.|[^"\\])*")\)|"(?:\\.|[^"\\])*"', text):
            if match.group(1):
                value = json.loads(match.group(1))
                result.setdefault(value, []).append((str(path.relative_to(root)), text.count("\n", 0, match.start()) + 1))
    return result


def validate(path=CATALOG, expected=None, language="fr_FR"):
    """Contrôle les textes manquants et les paramètres Qt, sans installer Linguist pour les tests."""
    tree = ET.parse(path)
    errors, known = [], set()
    if tree.getroot().get("language") != language:
        errors.append(f"Catalogue attendu : {language}")
    if tree.getroot().get("sourcelanguage") != "en_US":
        errors.append("Langue source attendue : en_US")
    for context in tree.findall("context"):
        name = context.findtext("name")
        for message in context.findall("message"):
            source = message.findtext("source", "")
            target = message.find("translation")
            if name == "Gad":
                known.add(source)
            if target is None or target.get("type") == "unfinished" or not target.text:
                errors.append(f"Traduction absente : {source}")
                continue
            placeholders = lambda text: sorted(re.findall(r"%L?\d+|%n", text))
            if placeholders(source) != placeholders(target.text):
                errors.append(f"Paramètres différents : {source}")
            if re.findall(r'href="([^"]+)"', source) != re.findall(r'href="([^"]+)"', target.text):
                errors.append(f"Lien modifié : {source}")
    if expected is not None:
        errors.extend(f"Texte absent du catalogue : {source}" for source in sorted(set(expected) - known))
    return errors


def fingerprint(catalog=CATALOG):
    """Associe la source et le binaire versionnés pour détecter un oubli de recompilation."""
    return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (catalog, catalog.with_suffix(".qm"))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Vérifie sans modifier ni invoquer les outils Qt")
    args = parser.parse_args()
    marked = sources()
    for catalog in sorted((ROOT / "translations").glob("gad_*.ts")):
        language = catalog.stem.removeprefix("gad_")
        stamp = catalog.with_suffix(".sha256.json")
        if not args.check:
            tree = ET.parse(catalog)
            context = next(c for c in tree.findall("context") if c.findtext("name") == "Gad")
            known = {m.findtext("source"): m for m in context.findall("message")}
            for source, locations in marked.items():
                message = known.get(source)
                if message is None:
                    message = ET.SubElement(context, "message")
                    ET.SubElement(message, "source").text = source
                    ET.SubElement(message, "translation", type="unfinished")
                for old in message.findall("location"):
                    message.remove(old)
                for filename, line in locations:
                    message.insert(0, ET.Element("location", filename="../" + filename, line=str(line)))
            ET.indent(tree)
            tree.write(catalog, encoding="utf-8", xml_declaration=True)
        errors = validate(catalog, expected=marked, language=language)
        if errors:
            raise SystemExit("\n".join(errors))
        if args.check:
            if not stamp.exists() or json.loads(stamp.read_text()) != fingerprint(catalog):
                raise SystemExit("Catalogue modifié : lancer make translations après traduction dans Qt Linguist.")
        else:
            tool = shutil.which("lrelease6") or shutil.which("lrelease")
            if not tool:
                raise SystemExit("Installer Qt Linguist (lrelease6) pour compiler les traductions.")
            subprocess.run([tool, str(catalog), "-qm", str(catalog.with_suffix(".qm"))], check=True)
            stamp.write_text(json.dumps(fingerprint(catalog), indent=2) + "\n")
        print(f"Traductions : {len(marked)} textes Qt vérifiés, catalogue {language} à jour.")



if __name__ == "__main__":
    main()
