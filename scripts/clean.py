#!/usr/bin/env python3
"""Nettoyer les fichiers générés du projet, sans toucher aux données de Gad."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

PROJECT = Path(__file__).resolve().parent.parent


def candidates(root):
    """Ne sélectionner que les sorties connues, en conservant les fichiers Git et releases."""
    root = Path(root).resolve()
    result = subprocess.run(['git', '-C', str(root), 'ls-files', '-z'],
                            check=True, capture_output=True)
    tracked = [root / os.fsdecode(name) for name in result.stdout.split(b'\0') if name]
    selected = set()

    def add(path):
        # Un répertoire contenant un fichier versionné ne peut pas être supprimé en bloc.
        if not any(path == item or path in item.parents for item in tracked):
            selected.add(path)

    target = root / 'target'
    if target.is_symlink():
        add(target)  # Supprimer le lien seulement ; ne jamais parcourir sa destination.
    elif target.is_dir():
        for path in target.iterdir():
            if path.name != 'releases':
                add(path)
    for name in ['gad', 'cargo-home', '.debhelper', 'files', 'debhelper-build-stamp']:
        path = root / 'debian' / name
        if path.exists() or path.is_symlink():
            add(path)
    # Ne pas suivre les liens ni explorer Git, les builds et les releases conservées.
    for directory, folders, files in os.walk(root, followlinks=False):
        base = Path(directory)
        folders[:] = [name for name in folders if not
                      (base == root and name in {'.git', 'target'})]
        for name in list(folders):
            if name in {'__pycache__', '.pytest_cache', '.mypy_cache', '.ruff_cache'}:
                add(base / name)
                folders.remove(name)
        for name in files:
            path = base / name
            if name.endswith(('.pyc', '.pyo', '.swp', '.swo', '.kate-swp')):
                add(path)
            elif base == root and (name.endswith(('.deb', '.ddeb', '.buildinfo', '.changes', '.dsc', '.build', '.upload'))
                                   or (name.startswith('gad_') and ('.orig.tar.' in name or '.debian.tar.' in name))):
                add(path)
            elif base == root / 'debian' and name.endswith(('.substvars', '.debhelper.log')):
                add(path)
    return sorted((path for path in selected if not any(parent in selected for parent in path.parents)),
                  key=lambda path: str(path.relative_to(root)))


def clean(root, apply=False):
    """Afficher la sélection ; --apply autorise sa suppression dans ce seul dépôt."""
    root = Path(root).resolve()
    paths = candidates(root)
    for path in paths:
        print(('Suppression : ' if apply else 'À supprimer : ') + str(path.relative_to(root)))
        if apply:
            if path.is_symlink() or path.is_file():
                path.unlink()
            else:
                shutil.rmtree(path)
    print(f'{len(paths)} élément(s). ' + ('Nettoyage terminé.' if apply else 'Aperçu : utiliser --apply pour nettoyer.'))
    print('Conservés : target/releases/, fichiers versionnés et données personnelles de Gad.')
    return paths


def main():
    """Le mode par défaut permet de contrôler la liste avant toute suppression."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='Supprimer les fichiers affichés.')
    args = parser.parse_args()
    clean(PROJECT, args.apply)


if __name__ == '__main__':
    main()
