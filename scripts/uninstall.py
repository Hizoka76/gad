#!/usr/bin/env python3
"""Retire les fichiers installés et le raccourci ; conserve volontairement le journal et les sauvegardes de restauration."""
from pathlib import Path
from xdg_paths import data_home, config_home
import shutil
import subprocess
from dolphin_shortcut import configure
from dolphin_integration import restore_launcher

from krusader_shortcut import configure as configure_krusader


def uninstall():
    """Restaurer les intégrations avant de retirer le programme, sans toucher à son journal."""
    # Respecter le même emplacement XDG que lors de l’installation.
    data_dir = data_home()
    # Réactiver le collage Krusader avant de retirer l’exécutable qu’il utilise.
    configure_krusader(False, Path.home() / ".local/bin/gad")
    # Restaurer le collage avant de retirer l’action qui le remplace ; un reçu invalide arrête le retrait sans casser Ctrl+V.
    configure(remove=True)
    restore_launcher()
    shutil.rmtree(data_dir / "gad/dolphin",ignore_errors=True)
    for path in [config_home() / "autostart/org.hizoka.gad.desktop", Path.home() / ".local/bin/gad", data_dir / "applications/gad.desktop", data_dir / "applications/org.hizoka.gad.desktop", data_dir / "kio/servicemenus/gad-paste.desktop", data_dir / "kio/servicemenus/gad-paste-current.desktop"]:
        path.unlink(missing_ok=True)
        print(f"Supprimé : {path}")
    # Retirer les noms d’icônes historiques et actuels aux seules tailles installées.
    for size in (16, 22, 24, 32, 48, 64, 128, 256, 512):
        for name in ("gad", "gad-boucles-document"):
            (data_dir / f"icons/hicolor/{size}x{size}/apps/{name}.png").unlink(missing_ok=True)
    # Le retrait ciblé du raccourci préserve les réglages personnels de Dolphin.
    # Le retour aux raccourcis d’origine précède le retrait de l’outil et de son reçu.
    (data_dir / "gad/dolphin-paste.json").unlink(missing_ok=True)
    for name in ("dolphin_shortcut.py", "krusader_shortcut.py", "xdg_paths.py"):
        (data_dir / "gad/integration" / name).unlink(missing_ok=True)
    # Le lanceur Konqueror dédié ne doit pas rester lié à un module Gad désinstallé.
    for path in [Path.home() / ".local/bin/gad-konqueror",
                 data_dir / "applications/org.hizoka.gad.konqueror.desktop",
                 data_dir / "konqueror/partsrcfiles/gad_konqueror.rc",
                 data_dir / "gad/krusader-paste.json"]:
        path.unlink(missing_ok=True)
    shutil.rmtree(data_dir / "gad/konqueror", ignore_errors=True)
    # Retirer aussi l’association gad:// du cache MIME, comme lors de l’installation.
    if cache := shutil.which("update-desktop-database"):
        subprocess.run([cache, str(data_dir / "applications")], check=False)
    if cache := shutil.which("kbuildsycoca6"):
        subprocess.run([cache, "--noincremental"], check=False)


if __name__ == "__main__":
    uninstall()
