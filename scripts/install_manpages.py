#!/usr/bin/env python3
"""Installer les manuels dans un répertoire man, sans lancer de programme graphique."""
from pathlib import Path
import shutil
from generate_manpages import LANGUAGES, filename

ROOT = Path(__file__).resolve().parent.parent


def install(destination):
    """Copier les pages générées avec les noms attendus par man(1)."""
    for name in ('gad',):
        for language in LANGUAGES:
            original = ROOT / 'packaging' / filename(name, language)
            folder = destination / (language if language != 'en' else '') / 'man1'
            folder.mkdir(parents=True, exist_ok=True)
            target = folder / (name + '.1')
            staged = target.with_name('.' + target.name + '.install')
            shutil.copyfile(original, staged)
            staged.chmod(0o644)
            staged.replace(target)
