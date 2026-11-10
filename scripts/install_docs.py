"""Installe les guides HTML/Markdown déjà générés et les images des sources Markdown."""
import json
from pathlib import Path
from doc_catalog import GUIDES, LANGUAGES, portal, runtime_catalog


def install(destination, source=None):
    """Publie les fichiers connus atomiquement et préserve les documents personnels."""
    source=source or Path(__file__).resolve().parent.parent/'docs'
    catalog=runtime_catalog();copied=0
    def copy(relative):
        original=source/relative
        if not original.is_file():return False
        target=destination/relative;target.parent.mkdir(parents=True,exist_ok=True)
        data=original.read_bytes()
        if not target.exists() or target.read_bytes()!=data:
            staged=target.with_name('.'+target.name+'.install');staged.write_bytes(data);staged.replace(target)
        return True
    for guide in catalog['guides']:
        for kind in ('html','md'):copied+=int(copy(guide[kind]))
    for language in LANGUAGES:copy(portal(language))
    for image in (source/'images').rglob('*'):
        if image.is_file() and image.suffix in ('.png','.svg','.json'):copy(image.relative_to(source))
    # Retirer uniquement les anciennes éditions générées par Gad, sans suivre un dossier lié.
    old=destination/'pdf'
    if old.is_dir() and not old.is_symlink():
        for guide in GUIDES:
            obsolete=old/(guide.slug+'.pdf')
            if obsolete.is_file() or obsolete.is_symlink():obsolete.unlink()
        try:old.rmdir()
        except OSError:pass
    destination.mkdir(parents=True,exist_ok=True)
    staged=destination/'.catalog.json.install';staged.write_text(json.dumps(catalog,ensure_ascii=False,indent=2)+'\n');staged.replace(destination/'catalog.json')
    print(f'Documentation installée : {copied} fichiers HTML/Markdown')
