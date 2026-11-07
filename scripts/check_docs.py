#!/usr/bin/env python3
"""Vérifie les liens locaux, ancres, captures et exports des guides de Gad."""
import base64
import hashlib
import json
import re
import struct
from pathlib import Path
from urllib.parse import unquote
from doc_catalog import GUIDES, LANGUAGES, LABELS, portal, runtime_catalog

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"


def main():
    """Échoue sur un document absent, une ancre invalide ou une capture altérée."""
    import markdown
    errors = []
    markdown_cache = {}
    html_cache = {}
    sources=[*(ROOT / portal(lang) for lang in LANGUAGES),*(ROOT / ("TESTING.md" if lang=="fr" else "TESTING."+lang+".md") for lang in LANGUAGES),*(DOCS / portal(lang) for lang in LANGUAGES),*(DOCS / guide.source for guide in GUIDES)]
    for source in sources:
        for target in re.findall(r'\]\(([^)]+)\)', source.read_text()):
            if re.match(r'^[a-z]+:', target):
                continue
            filename, _, anchor = target.partition("#")
            path = (source.parent / unquote(filename)).resolve() if filename else source
            if not path.exists():
                errors.append(f"{source.name} : lien absent {target}")
            elif anchor and path.suffix == ".md":
                if path not in markdown_cache:
                    markdown_cache[path] = markdown.markdown(path.read_text(), extensions=["toc", "tables", "fenced_code", "attr_list"])
                html = markdown_cache[path]
                if f'id="{anchor}"' not in html:
                    errors.append(f"{source.name} : ancre absente {target}")
    assert json.loads((DOCS / "catalog.json").read_text()) == runtime_catalog(), "Catalogue de documentation périmé"
    manifests={}
    for language in LANGUAGES:
        directory=DOCS / "images" / language
        manifest=json.loads((directory / "captures.json").read_text());manifests[language]=manifest
        assert len(manifest["captures"])==14, f"Quatorze captures attendues : {language}"
        assert manifest["locale"]==LABELS[language]["locale"], f"Langue des captures : {language}"
        assert manifest.get("theme")=="light", f"Thème clair attendu : {language}"
        assert sum(int(manifest["window_color"][i:i+2],16) for i in (1,3,5))>540, f"Palette sombre : {language}"
        for capture in manifest["captures"]:
            data=(directory / capture["file"]).read_bytes()
            assert data[:8]==b"\x89PNG\r\n\x1a\n"
            assert struct.unpack(">II",data[16:24])==(capture["width"],capture["height"])
            assert hashlib.sha256(data).hexdigest()==capture["sha256"]
    for guide in GUIDES:
        slug=guide.slug
        html = (DOCS / "html" / (slug + ".html")).read_text()
        assert f'lang="{guide.language}"' in html and 'src="images/' not in html
        for target in re.findall(r'href="([^"]+)"', html):
            if re.match(r'^[a-z]+:', target):
                continue
            filename, _, anchor = target.partition("#")
            path = (DOCS / "html" / unquote(filename)).resolve() if filename else DOCS / "html" / (slug + ".html")
            if not path.exists():
                errors.append(f"{slug}.html : lien absent {target}")
            elif anchor and path.suffix == ".html":
                if path not in html_cache:html_cache[path] = path.read_text()
                if f'id="{anchor}"' not in html_cache[path]:errors.append(f"{slug}.html : ancre absente {target}")
        if guide.audience == "user":
            for capture in manifests[guide.language]["captures"]:
                encoded = base64.b64encode((DOCS / "images" / guide.language / capture["file"]).read_bytes()).decode()
                assert encoded in html, f"Capture exportée périmée : {capture['file']}"
        assert (DOCS / "pdf" / (slug + ".pdf")).read_bytes().startswith(b"%PDF")
    if errors:
        raise SystemExit("\n".join(errors))
    capture_count=sum(len(manifest["captures"]) for manifest in manifests.values())
    print(f"Documentation vérifiée : liens, ancres, {capture_count} captures et {len(GUIDES)*2} exports ({len(LANGUAGES)} langues).")


if __name__ == "__main__":
    main()
