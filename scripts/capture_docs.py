#!/usr/bin/env python3
"""Produit des captures des vrais widgets Gad dans une instance de démonstration hors écran et isolée."""
from datetime import datetime, timezone
import argparse
import configparser
import hashlib
import json
import os
from resources import generate
from doc_catalog import LANGUAGES, LABELS
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    """Ne lance jamais le moteur réel ni l’instance du bureau ; seuls les chemins de démonstration sont créés."""
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--language",choices=LANGUAGES,action="append",help="Langue à capturer ; toutes par défaut.")
    args=parser.parse_args()
    languages=args.language or LANGUAGES
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "Qt6Widgets", "Qt6Network", "Qt6DBus", "KF6WindowSystem"], text=True))
    qt_tools = Path(subprocess.check_output(["qmake6", "-query", "QT_INSTALL_LIBEXECS"], text=True).strip())
    version = re.search(r'^version\s*=\s*"([^"]+)"', (ROOT / "Cargo.toml").read_text(), re.M).group(1)
    date = datetime.now(timezone.utc).date().isoformat()
    with tempfile.TemporaryDirectory(prefix="gad-doc-build-") as temp:
        build = Path(temp)
        resource = build / "gad_assets.cpp"
        moc = build / "moc_notifications.cpp"
        generate(resource.with_suffix(".qrc"))
        subprocess.run([qt_tools / "rcc", "--name", "gad_assets", resource.with_suffix(".qrc"), "-o", resource], check=True)
        subprocess.run([qt_tools / "moc", ROOT / "native/notifications.h", "-o", moc], check=True)
        binary = build / "capture-docs"
        subprocess.run(["c++", "-std=c++17", "-O1", "-fPIC", "-Wall", "-Wextra", "-I", ROOT / "native",
                        f'-DGAD_VERSION="{version}"', f'-DGAD_BUILD_DATE="{date}"', ROOT / "docs/tools/capture.cpp",
                        *sorted((ROOT / "native").glob("*.cpp")), resource, moc, "-o", binary, *flags], check=True)
        user_config=Path(os.environ.get("XDG_CONFIG_HOME",Path.home()/".config"))
        for language in languages:
            output=ROOT / "docs/images" / language;output.mkdir(parents=True,exist_ok=True)
            private=build / language;private.mkdir();runtime=private / "runtime";runtime.mkdir(mode=0o700)
            home=private / "home";home.mkdir();config=private / "config";config.mkdir()
            # Les vrais exports restent en lecture seule ; seul le lien vit dans les données de capture.
            documentation=private / "data/gad/docs";documentation.parent.mkdir(parents=True);documentation.symlink_to(ROOT / "docs",target_is_directory=True)
            # Reproduire l’installation du composant privé sans appeler l’intégration du bureau réel.
            integration=private / "data/gad/integration";integration.mkdir()
            for name in ("dolphin_shortcut.py","xdg_paths.py"):
                shutil.copy2(ROOT / "scripts" / name,integration / name)
            # Le thème clair est imposé dans la seule configuration de démonstration.
            palette=configparser.ConfigParser(interpolation=None,strict=False);palette.optionxform=str
            palette.read(user_config / "kdeglobals")
            palette.read("/usr/share/color-schemes/BreezeLight.colors")
            if not palette.has_section("General"):palette.add_section("General")
            palette["General"]["ColorScheme"]="BreezeLight"
            if not palette.has_section("KDE"):palette.add_section("KDE")
            palette["KDE"]["widgetStyle"]="Breeze"
            with (config / "kdeglobals").open("w") as stream:palette.write(stream)
            platform_config=private / "offscreen.json"
            platform_config.write_text(json.dumps({"screens":[{"name":"Gad documentation","x":0,"y":0,"width":3840,"height":2160,"logicalDpi":96}]}))
            env=dict(os.environ,HOME=str(home),QT_QPA_PLATFORM="offscreen:configfile="+str(platform_config),QT_QPA_PLATFORMTHEME="kde",QT_STYLE_OVERRIDE="Breeze",QT_AUTO_SCREEN_SCALE_FACTOR="0",QT_SCALE_FACTOR="2",GAD_LANGUAGE=LABELS[language]["locale"],
                     XDG_DATA_DIRS="/usr/local/share:/usr/share",XDG_CONFIG_HOME=str(config),XDG_DATA_HOME=str(private/"data"),XDG_CACHE_HOME=str(private/"cache"),XDG_RUNTIME_DIR=str(runtime))
            subprocess.run(["dbus-run-session","--",binary,output,LABELS[language]["locale"]],env=env,check=True,timeout=45)
            manifest=json.loads((output / "captures.json").read_text());manifest.update(version=version,generated_at=date,language=language)
            for capture in manifest["captures"]:capture["sha256"]=hashlib.sha256((output/capture["file"]).read_bytes()).hexdigest()
            (output/"captures.json").write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+"\n")
            print(f"{language}: {len(manifest['captures'])} captures claires",flush=True)
    if set(languages)==set(LANGUAGES):
        for legacy in (ROOT/"docs/images").glob("[0-9][0-9]-*.png"):legacy.unlink()
        (ROOT/"docs/images/captures.json").unlink(missing_ok=True)


if __name__ == "__main__":
    main()
