#!/usr/bin/env python3
"""Installation DESTDIR pour dpkg : aucune modification du compte utilisateur."""
import argparse
from pathlib import Path
import shutil
from install_docs import install as install_docs

PROJECT = Path(__file__).resolve().parent.parent


def install(destdir, binary):
    """Copier uniquement les fichiers distribués dans un arbre de paquet isolé."""
    root = Path(destdir).resolve()
    def copy(source, relative, mode=0o644):
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(mode)
    copy(binary, "usr/bin/gad", 0o755)
    copy(PROJECT / "packaging/gad-dolphin-paste", "usr/bin/gad-dolphin-paste", 0o755)
    copy(PROJECT / "packaging/org.hizoka.gad.desktop", "usr/share/applications/org.hizoka.gad.desktop")
    for icon in (PROJECT / "packaging/icons").glob("*/apps/*.png"):
        copy(icon, "usr/share/icons/hicolor/" + str(icon.relative_to(PROJECT / "packaging/icons")))
    for name in ("dolphin_shortcut.py", "xdg_paths.py"):
        copy(PROJECT / "scripts" / name, "usr/share/gad/integration/" + name)
    for name in ("gad-paste.desktop", "gad-paste-current.desktop"):
        copy(PROJECT / "packaging" / name, "usr/share/kio/servicemenus/" + name)
    # Les notices accompagnent aussi le binaire, pas seulement l’archive source.
    for crate in (PROJECT / "vendor").glob("*"):
        for notice in crate.iterdir():
            if notice.is_file() and (notice.name.upper().startswith(("LICENSE", "COPYING", "UNLICENSE")) or notice.name == "Cargo.toml"):
                copy(notice, "usr/share/doc/gad/third-party/" + crate.name + "/" + notice.name)
    install_docs(root / "usr/share/gad/docs")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destdir", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    install(args.destdir, args.binary)
