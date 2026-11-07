#!/usr/bin/env python3
"""Teste les raccourcis avec KXMLGUI/Qt réels, un bus privé et un presse-papiers hors écran."""
import os
from pathlib import Path
import subprocess
import tempfile
from dolphin_shortcut import configure, configure_paste

ROOT = Path(__file__).resolve().parent.parent


def main():
    """Aucun Dolphin ni fichier de configuration de la session n’est utilisé."""
    with tempfile.TemporaryDirectory(prefix="gad-dolphin-integration-") as tmp:
        base = Path(tmp)
        runtime = base / "runtime"
        runtime.mkdir(mode=0o700)
        env = dict(os.environ, HOME=str(base), XDG_CONFIG_HOME=str(base / "config"),
                   XDG_DATA_HOME=str(base / "data"), XDG_RUNTIME_DIR=str(runtime),
                   QT_QPA_PLATFORM="offscreen")
        # La compilation CMake emploie les dépendances transitives de KDE plutôt que des chemins d’en-tête supposés.
        source = base / "source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.16)
project(GadDolphinPasteTest LANGUAGES CXX)
find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
find_package(KF6XmlGui REQUIRED)
find_package(KF6KIO REQUIRED)
add_executable(test "''' + str(ROOT / "tests/dolphin_paste.cpp") + '''")
target_link_libraries(test PRIVATE Qt6::Widgets Qt6::Test KF6::XmlGui KF6::KIOCore KF6::Service)
''')
        subprocess.run(["cmake", "-S", source, "-B", base / "build"], check=True)
        subprocess.run(["cmake", "--build", base / "build"], check=True)
        # La configuration est modifiée dans l’environnement temporaire avant de lancer la vraie lecture KXMLGUI.
        from unittest.mock import patch
        with patch.dict(os.environ, env):
            configure_paste(True)
            path = base / "data/kxmlgui5/dolphin/dolphinui.rc"
            subprocess.run(["dbus-run-session", "--", base / "build/test", path], env=env, check=True, timeout=15)
            configure_paste(False)
            subprocess.run(["dbus-run-session", "--", base / "build/test", path, "--restored"], env=env, check=True, timeout=15)
        print("OK : raccourcis Gad/KIO, collage de texte et restauration, avec KXMLGUI réel.")


if __name__ == "__main__":
    main()
