#!/usr/bin/env python3
"""Construit le module Konqueror et installe son lanceur avec un chemin de plugins propre à cette application."""
from pathlib import Path
import json
import shlex
import shutil
import subprocess
from xdg_paths import data_home

ROOT = Path(__file__).resolve().parent.parent


def build(binary):
    """Utilise les en-têtes Konqueror installés ; un SDK extrait est accepté pour les essais isolés."""
    candidates = [Path('/usr'), ROOT / 'target/konqueror-sdk/usr']
    prefix = next((p for p in candidates if (p / 'include/KF6/konq_kpart_plugin.h').exists()), None)
    if prefix is None:
        return None
    output = ROOT / 'target/konqueror-plugin'
    output.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'native/konqueror'
    tools = Path(subprocess.check_output(['qmake6','-query','QT_INSTALL_LIBEXECS'],text=True).strip())
    flags = shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Widgets','Qt6Xml','Qt6DBus'],text=True))
    include = [f'-I{prefix}/include/KF6', '-I/usr/include/KF6/KCoreAddons','-I/usr/include/KF6/KXmlGui']
    subprocess.run([tools/'moc',*include,*[f for f in flags if f.startswith(('-I','-D'))],source/'plugin.cpp','-o',output/'plugin.moc'],check=True)
    libraries = list((prefix/'lib').glob('**/libKF6Konq.so'))
    if not libraries:
        raise RuntimeError('Bibliothèque Konqueror introuvable')
    target = output/'gad_konqueror.so'
    subprocess.run(['c++','-std=c++17','-O2','-fPIC','-shared',*include,f'-I{output}',
                    '-DGAD_BINARY='+json.dumps(str(binary)),source/'plugin.cpp','-o',target,
                    *flags,str(libraries[0]),'-lKF6CoreAddons','-lKF6XmlGui'],check=True)
    return target


def install(binary):
    """Aucun changement du chemin global de plugins Qt : seul le lanceur de Konqueror reçoit le module."""
    plugin = build(binary)
    if plugin is None:
        print('Konqueror : menu contextuel disponible ; installer Konqueror puis relancer install.py pour activer Ctrl+V.')
        return
    data = data_home()
    directory = data/'gad/konqueror/plugins/konqueror/kpartplugins'
    directory.mkdir(parents=True,exist_ok=True)
    staged=directory/'.gad_konqueror.so.install';shutil.copyfile(plugin,staged);staged.replace(directory/plugin.name)
    rc=data/'konqueror/partsrcfiles/gad_konqueror.rc';rc.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(ROOT/'native/konqueror/gad_konqueror.rc',rc)
    launcher=Path.home()/'.local/bin/gad-konqueror'
    # Les chemins sont des arguments Python ; aucun shell n’interprète les URL ou le nom du programme.
    launcher.write_text('#!/usr/bin/env python3\nimport os,shutil,sys\nfrom pathlib import Path\n'
                        f'plugin_path={str(data/"gad/konqueror/plugins")!r}\n'
                        'program=shutil.which("konqueror")\n'
                        'if not program:sys.exit("Konqueror is not installed.")\n'
                        'os.environ["QT_PLUGIN_PATH"]=plugin_path+(os.pathsep+os.environ["QT_PLUGIN_PATH"] if os.environ.get("QT_PLUGIN_PATH") else "")\n'
                        'os.execv(program,[program,*sys.argv[1:]])\n')
    launcher.chmod(0o755)
    # L’entrée dédiée évite de masquer les réglages et le lanceur Konqueror de l’utilisateur.
    desktop=data/'applications/org.hizoka.gad.konqueror.desktop'
    desktop.parent.mkdir(parents=True,exist_ok=True)
    command='"'+str(launcher).replace('\\','\\\\').replace('"','\\"').replace('`','\\`').replace('$','\\$').replace('%','%%')+'"'
    desktop.write_text('[Desktop Entry]\nType=Application\nName=Konqueror with Gad\nName[fr]=Konqueror avec Gad\n'
                       f'Exec={command} %U\nIcon=konqueror\nTerminal=false\nCategories=System;FileManager;\n')
    print('Konqueror : utiliser le lanceur « Konqueror avec Gad » pour Ctrl+V ; les menus contextuels sont communs.')
