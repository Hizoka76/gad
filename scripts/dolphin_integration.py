#!/usr/bin/env python3
"""Construire et installer le module Qt de surveillance, sans SDK privé Dolphin."""
from pathlib import Path
import argparse
import re
import json
import os
import shlex
import shutil
import subprocess
from xdg_paths import data_home

ROOT = Path(__file__).resolve().parent.parent


def build():
    """Utiliser uniquement les interfaces publiques Qt, KXMLGUI et QGenericPlugin."""
    source = ROOT / 'native/dolphin'
    output = ROOT / 'target/dolphin-plugin'
    output.mkdir(parents=True, exist_ok=True)
    build_directory=output / 'cmake'
    subprocess.run(['cmake','-S',source,'-B',build_directory,'-DCMAKE_BUILD_TYPE=Release'],check=True)
    subprocess.run(['cmake','--build',build_directory,'--target','gad_dolphin_watchdog','--parallel','2'],check=True)
    target=output / 'libgad_dolphin_watchdog.so'
    generic=output / "plugins/generic";generic.mkdir(parents=True,exist_ok=True)
    shutil.copy2(target,generic / target.name)
    return target


def install():
    """Le chemin de plugins est privé au processus Dolphin lancé par Gad."""
    plugin = build()
    target = data_home()/'gad/dolphin/plugins/generic'/plugin.name
    target.parent.mkdir(parents=True, exist_ok=True)
    staged=target.with_name('.'+target.name+'.install')
    shutil.copy2(plugin,staged);staged.replace(target)


def protect_launcher(binary):
    """Surcharger le lanceur KDE dans le seul compte utilisateur, avec un reçu de restauration."""
    destination=data_home()/'applications/org.kde.dolphin.desktop'
    receipt=data_home()/'gad/dolphin-launcher.json'
    state=json.loads(receipt.read_text()) if receipt.exists() else None
    if state:
        if not destination.exists() or destination.read_text()!=state['installed']:
            raise RuntimeError('Le lanceur Dolphin a été modifié ; sa version actuelle est conservée.')
        original=state['original']
        source=state['source']
    else:
        original=destination.read_text() if destination.exists() else None
        sources=[Path(part)/'applications/org.kde.dolphin.desktop' for part in os.environ.get('XDG_DATA_DIRS','/usr/local/share:/usr/share').split(':') if part and Path(part).is_absolute()]
        source=original if original is not None else next((path.read_text() for path in sources if path.is_file()),None)
        if source is None:raise RuntimeError('Lanceur Dolphin introuvable.')
    # Les champs Exec obéissent au format Desktop, sans interprétation par un shell.
    value=str(binary)
    for char in ('\\','"','`','$'):value=value.replace(char,'\\'+char)
    quoted='"'+value.replace('\\','\\\\').replace('%','%%')+'"'
    lines=source.splitlines();group='';changed=False
    for i,line in enumerate(lines):
        if line.startswith('['):group=line
        if group=='[Desktop Entry]' and line.startswith('Exec='):
            args=shlex.split(line[5:])
            if not args or Path(args[0]).name!='dolphin':raise RuntimeError('Lanceur Dolphin personnalisé : commande conservée.')
            # Conserver les arguments et marqueurs %u/%U du lanceur original.
            tail=re.match(r'^(?:"(?:\\.|[^"])*"|[^\s]+)(.*)$',line[5:].strip()).group(1).strip()
            lines[i]='Exec='+quoted+' --dolphin'+(' '+tail if tail else '');changed=True
        if group=='[Desktop Entry]' and line.startswith('DBusActivatable='):lines[i]='DBusActivatable=false'
    if not changed:raise RuntimeError('Commande du lanceur Dolphin introuvable.')
    installed='\n'.join(lines)+'\n'
    destination.parent.mkdir(parents=True,exist_ok=True);receipt.parent.mkdir(parents=True,exist_ok=True)
    state={'version':1,'original':original,'source':source,'installed':installed}
    staged=receipt.with_suffix('.tmp');staged.write_text(json.dumps(state,ensure_ascii=False,indent=2)+'\n');staged.replace(receipt)
    staged=destination.with_suffix('.tmp');staged.write_text(installed);staged.chmod(0o644);staged.replace(destination)


def restore_launcher():
    """Restaurer seulement le lanceur encore détenu par Gad ; préserver les changements ultérieurs."""
    destination=data_home()/'applications/org.kde.dolphin.desktop';receipt=data_home()/'gad/dolphin-launcher.json'
    if not receipt.exists():return
    state=json.loads(receipt.read_text())
    if destination.exists() and destination.read_text()==state['installed']:
        if state['original'] is None:destination.unlink()
        else:destination.write_text(state['original'])
    receipt.unlink()


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install',action='store_true')
    parser.add_argument('--protect-launcher',type=Path,metavar='GAD_BINARY')
    args=parser.parse_args()
    install() if args.install else build()
    if args.protect_launcher:protect_launcher(args.protect_launcher)
