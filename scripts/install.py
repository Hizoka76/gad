#!/usr/bin/env python3
"""Construit et installe Gad dans le compte utilisateur, avec son lanceur, ses icônes et son action Dolphin."""
import os
import json
from pathlib import Path
from xdg_paths import data_home
import shutil
import subprocess
from dolphin_integration import install as install_dolphin, protect_launcher
from install_docs import install as install_documentation
from install_manpages import install as install_manuals
from dolphin_shortcut import configure
from konqueror_integration import install as install_konqueror

# Construire d’abord le binaire ; une erreur de compilation laisse l’installation existante disponible.
project = Path(__file__).resolve().parent.parent
subprocess.run(["cargo", "build", "--release", "--locked"], cwd=project, check=True)
bin_dir = Path.home() / ".local/bin"
data_dir = data_home()
bin_dir.mkdir(parents=True, exist_ok=True)
binary = bin_dir / "gad"
# Le renommage remplace le fichier installé sans réécrire un exécutable potentiellement déjà chargé.
staged = bin_dir / ".gad.install"
shutil.copy2(project / "target/release/gad", staged)
staged.chmod(0o755)
staged.replace(binary)


def executable(path):
    """Encode un chemin pour le champ Exec d’une entrée Desktop.

    Les règles d’arguments et les échappements du format Desktop sont deux étapes distinctes.
    """
    # Exec argument quoting, then Desktop Entry value escaping.
    value = str(path)
    for char in ['\\', '"', '`', '$']:
        value = value.replace(char, '\\' + char)
    return '"' + value.replace('\\', '\\\\') + '"'


# Le lanceur principal porte l’identifiant de fenêtre Wayland ; l’action KIO transmet le contexte local.
entries = {
    data_dir / "applications/org.hizoka.gad.desktop": f'''[Desktop Entry]
Type=Application
Name=Gad
Comment=Advanced file transfer manager for KDE
Comment[fr]=Gad, il copie. — Gestionnaire de copies pour KDE
Exec={executable(binary)} %u
MimeType=x-scheme-handler/gad;
Icon={data_dir / "icons/hicolor/256x256/apps/gad-boucles-document.png"}
Terminal=false
Categories=Utility;FileTools;Qt;KDE;
StartupNotify=true
StartupWMClass=Gad
''',
    data_dir / "kio/servicemenus/gad-paste.desktop": f'''[Desktop Entry]
Type=Service
MimeType=inode/directory;application/octet-stream;
Actions=GadPaste;
X-KDE-Protocols=file,ftp,sftp,smb,kdeconnect
X-KDE-Priority=TopLevel

[Desktop Action GadPaste]
Name=Paste with Gad
Name[fr]=Coller avec Gad
Icon={data_dir / "icons/hicolor/256x256/apps/gad-boucles-document.png"}
Exec={executable(binary)} --paste-context %U
''',
}
# Le raccourci lit la fenêtre active ; le clic droit garde son contexte explicite, indépendant de la sélection mémorisée.
entries[data_dir / "kio/servicemenus/gad-paste-current.desktop"] = f'''[Desktop Entry]
Type=Service
MimeType=inode/directory;application/octet-stream;
Actions=GadPasteCurrent;
X-KDE-Priority=TopLevel

[Desktop Action GadPasteCurrent]
Name=Paste into the open folder with Gad
Name[fr]=Coller dans le dossier ouvert avec Gad
Icon={data_dir / "icons/hicolor/256x256/apps/gad-boucles-document.png"}
Exec={executable(binary)} --paste-current %U
'''
# Keep the historical launcher ID usable for existing shortcuts and pins.
entries[data_dir / "applications/gad.desktop"] = entries[data_dir / "applications/org.hizoka.gad.desktop"] + "NoDisplay=true\n"
# Qt génère les tailles d’icônes à partir de la même image que la fenêtre, hors écran.
export = subprocess.run([str(binary), "--export-icons", str(data_dir / "icons/hicolor")],
                        env=dict(os.environ, QT_QPA_PLATFORM="offscreen"), capture_output=True, text=True, check=True)
entries_changed = json.loads(export.stdout)["changed"] > 0
# Ne réécrire que les entrées modifiées limite les invalidations des caches de KDE.
for path, contents in entries.items():
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != contents:
        path.write_text(contents)
        entries_changed = True
    mode = 0o755 if "servicemenus" in path.parts else 0o644
    if path.stat().st_mode & 0o777 != mode:
        path.chmod(mode)
# Installer le gestionnaire réversible de raccourcis avec ses seules dépendances standard Python.
integration = data_dir / "gad/integration"
integration.mkdir(parents=True, exist_ok=True)
for name in ("dolphin_shortcut.py", "xdg_paths.py"):
    shutil.copy2(project / "scripts" / name, integration / name)
# Le raccourci appartient à Dolphin : aucun raccourci global concurrent n’est installé.
configure()
install_documentation(data_dir / "gad/docs")
install_manuals(data_dir / "man")
install_konqueror(binary)
install_dolphin()
if (data_dir / "gad/dolphin-launcher.json").exists():protect_launcher(binary)
entries_changed = True
# Le lien gad://show de la progression est associé au lanceur par sa déclaration MIME.
if entries_changed and (desktop_cache := shutil.which("update-desktop-database")):
    subprocess.run([desktop_cache, str(data_dir / "applications")], check=True)
# Le cache des services doit être rafraîchi après un changement d’icône ou de lanceur.
cache = shutil.which("kbuildsycoca6")
if cache and entries_changed:
    subprocess.run([cache, "--noincremental"], check=False)
# KService's launcher cache and KIconLoader's image cache are independent.
# Notify running Plasma panels after an icon or desktop entry changes.
if entries_changed and os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
    if notify := shutil.which("dbus-send"):
        for group in (0, 4):  # Desktop and Panel, as defined by KIconLoader::Group.
            subprocess.run([notify, "--session", "--type=signal", "/KIconLoader",
                            "org.kde.KIconLoader.iconChanged", f"int32:{group}"], check=False)
print(f"Installé : {binary}")
print("Dans Dolphin : Ctrl+C, puis clic droit sur le dossier cible → Coller avec Gad.")
print("Si Ctrl+V avec Gad est activé, Ctrl+V cible le dossier ouvert. Fermez puis rouvrez Dolphin pour charger les raccourcis.")
