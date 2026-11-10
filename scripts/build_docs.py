#!/usr/bin/env python3
"""Exporte les guides multilingues en HTML autonome."""
import argparse
import base64
import html
import json
import re
from pathlib import Path
from urllib.parse import urlsplit
from doc_catalog import GUIDES, LABELS, LANGUAGES, AUDIENCES, peers, runtime_catalog

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"


def image_uri(path):
    """Embarque une ressource locale afin que le document fonctionne hors ligne."""
    mime = "image/svg+xml" if path.suffix == ".svg" else "image/png"
    return "data:" + mime + ";base64," + base64.b64encode(path.read_bytes()).decode()


def rewrite_links(body):
    """Résout les guides et fragments depuis html/, sans réécrire les adresses externes."""
    outputs = {guide.source: guide.slug + ".html" for guide in GUIDES}

    def replace(match):
        target = match[1]
        if not target or target.startswith(("#", "/")) or urlsplit(target).scheme:
            return match[0]
        filename, separator, anchor = target.partition("#")
        path = outputs.get(filename, "../" + filename)
        return 'href="' + path + (separator + anchor if separator else "") + '"'

    return re.sub(r'href="([^"]*)"', replace, body)


def render_markdown(text):
    """Le rendu Markdown reste une dépendance d’export, pas des tests standard des scripts."""
    import markdown
    return markdown.markdown(text, extensions=["tables", "fenced_code", "toc", "sane_lists", "attr_list"])


def render_body(source, guide):
    """Structure les chapitres et figures avec une couverture dans la langue du guide."""
    body = render_markdown(source.read_text())
    heading = re.search(r'<h1[^>]*>(.*?)</h1>', body)
    if not heading:
        raise ValueError(f"Titre h1 absent : {source}")
    title = heading[1]
    body = body[:heading.start()] + body[heading.end():]
    intro, *chapters = re.split(r'(?=<h2\b)', body)
    labels = LABELS[guide.language]
    cover = f'<header class="cover"><img class="cover-icon" src="{image_uri(ROOT / "assets/icons/gad.png")}" alt="{labels["icon"]}"><div class="eyebrow">Gestionnaire Avancé de Duplication</div><h1>{title}</h1><p class="cover-subtitle">{labels[guide.audience]}</p>{intro}<p class="cover-meta">{labels["meta"]}</p></header>'
    body = cover + ''.join('<section class="chapter' + (' contents' if any(f'id="{anchor}"' in chapter[:140] for anchor in ("contents","sommaire")) else '') + (' troubleshooting' if f'id="{labels["troubleshooting"]}"' in chapter[:140] else '') + '">' + chapter + '</section>' for chapter in chapters)
    body = re.sub(r'<p>(<img\b[^>]+>)</p>\s*<p><em>(.*?)</em></p>', r'<figure>\1<figcaption>\2</figcaption></figure>', body, flags=re.S)
    body = re.sub(r'<p>(<img\b[^>]+>)</p>', r'<figure>\1</figure>', body)
    manifest = json.loads((DOCS / "images" / guide.language / "captures.json").read_text())
    widths = {item["file"]: item["width"] / manifest.get("device_pixel_ratio", 2) for item in manifest["captures"]}

    def embed(match):
        path = DOCS / match[1]
        width = widths.get(path.name, 1000)
        return f'src="{image_uri(path)}" style="width:{width:g}px;max-width:100%"'

    body = re.sub(r'src="(images/[^\"]+)"', embed, body)
    return rewrite_links(body)


def page_html(guide, body, css):
    """HTML autonome et navigation localisés."""
    labels = LABELS[guide.language]
    links = [(item.slug + ".html", item.title) for item in GUIDES if item.language == guide.language]
    links.extend((translated.slug + ".html",LABELS[translated.language]["language"]) for translated in peers(guide))
    nav = ''.join(f'<a href="{href}">{html.escape(label)}</a>' for href, label in links)
    if guide.audience == "quick":
        css += "\n@media print { .cover {min-height:0;padding:0 0 5mm;margin-bottom:5mm;break-after:auto;border-bottom:1px solid #dce6ed;} .cover-icon {width:17mm;height:17mm;float:right;} .cover-meta {display:none;} .cover-subtitle {font-size:12pt;} .cover h1 {font-size:25pt;} .chapter {break-before:auto;margin:0 0 5mm;} h2 {font-size:16pt;margin-top:5mm;} figure img {max-height:65mm;} }\n"
    return f'<!doctype html><html lang="{guide.language}"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="author" content="{labels["project"]}"><title>{html.escape("Gad — " + guide.title)}</title><style>{css}</style></head><body><nav>{nav}</nav><main>{body}</main></body></html>'


def main(guides=GUIDES):
    """Exporte les trois guides de chaque langue, sans modifier l’application."""
    css = (DOCS / "tools/documentation.css").read_text()
    (DOCS / "html").mkdir(exist_ok=True)
    for guide in guides:
        body = render_body(DOCS / guide.source, guide)
        output = DOCS / "html" / (guide.slug + ".html")
        output.write_text(page_html(guide, body, css))
        print(f"Export HTML : {guide.slug}")
    (DOCS / "catalog.json").write_text(json.dumps(runtime_catalog(),ensure_ascii=False,indent=2)+"\n")


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--language",choices=LANGUAGES,action="append")
    parser.add_argument("--audience",choices=AUDIENCES,action="append")
    args=parser.parse_args()
    main(tuple(guide for guide in GUIDES if (not args.language or guide.language in args.language) and (not args.audience or guide.audience in args.audience)))
