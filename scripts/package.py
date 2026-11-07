#!/usr/bin/env python3
"""Préparer des sources PPA autonomes et construire dans un répertoire isolé."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

PROJECT = Path(__file__).resolve().parent.parent


def prepare(output):
    """Limiter les sources aux fichiers du projet et verrouiller les crates embarquées."""
    debian_version = subprocess.check_output(
        ['dpkg-parsechangelog', '-l' + str(PROJECT / 'debian/changelog'), '-S', 'Version'],
        text=True).strip()
    # Cargo garde sa version ; le suffixe de test est propre au paquet Debian.
    version = debian_version.split(':', 1)[-1].rsplit('-', 1)[0]
    if not version or '/' in version or version in {'.', '..'}:
        raise SystemExit('Version Debian invalide pour une archive source.')
    output.mkdir(parents=True, exist_ok=True)
    source = output / ('gad-' + version)
    if source.exists():
        raise SystemExit(f'Répertoire déjà présent : {source}. Choisissez un autre --output.')
    source.mkdir()
    names = ['Cargo.toml', 'Cargo.lock', 'build.rs', 'Makefile', 'build.sh',
             'src', 'native', 'scripts', 'tests', 'translations', 'assets', 'docs', 'packaging']
    if (PROJECT / 'clean.sh').exists():
        names.append('clean.sh')
    names += [p.name for p in PROJECT.glob('README*.md')]
    names += [p.name for p in PROJECT.glob('TESTING*.md')]
    names += [p.name for p in PROJECT.glob('BUILDING*.md')]
    if (PROJECT / 'LICENSE').exists():
        names.append('LICENSE')
    for name in names:
        original = PROJECT / name
        if original.is_dir():
            shutil.copytree(original, source / name, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        else:
            shutil.copy2(original, source / name)
    # Cargo vérifie Cargo.lock et conserve les licences des dépendances dans vendor/.
    vendor = subprocess.run(['cargo', 'vendor', '--locked', '--versioned-dirs', 'vendor'],
                            cwd=source, check=True, capture_output=True, text=True)
    (source / '.cargo').mkdir()
    (source / '.cargo/config.toml').write_text(vendor.stdout)
    epoch = subprocess.check_output(['dpkg-parsechangelog', '-l' + str(PROJECT / 'debian/changelog'),
                                    '-S', 'Timestamp'], text=True).strip()
    archive = output / f'gad_{version}.orig.tar.xz'
    # L’archive upstream ne contient pas debian/, ajouté ensuite pour dpkg-source.
    with tarfile.open(archive, 'w:xz') as tar:
        def normalized(info):
            info.uid = info.gid = 0
            info.uname = info.gname = 'root'
            info.mtime = int(epoch)
            return info
        tar.add(source, arcname=source.name, filter=normalized)
    shutil.copytree(PROJECT / 'debian', source / 'debian',
                    ignore=shutil.ignore_patterns('gad', 'cargo-home', '*.debhelper*', '*.substvars', 'files'))
    return source


def main():
    """Ne signer ni envoyer automatiquement ; les artefacts restent inspectables."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=['prepare', 'source', 'binary'])
    parser.add_argument('--no-check-builddeps', action='store_true',
                        help='Test local avec rustup ; ne valide pas les dépendances du PPA.')
    parser.add_argument('--output', type=Path, default=None)
    args = parser.parse_args()
    output = args.output or PROJECT / ('target/packages-' + args.kind)
    source = prepare(output.resolve())
    if args.kind != 'prepare':
        command = ['dpkg-buildpackage', '-S' if args.kind == 'source' else '-b', '-us', '-uc']
        if args.kind == 'source' or args.no_check_builddeps:
            command.append('-d')
        subprocess.run(command, cwd=source, check=True, env=dict(os.environ, CARGO_NET_OFFLINE='true'))
    print(f'Sources et paquets : {source.parent}')


if __name__ == '__main__':
    main()
