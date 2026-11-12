#!/usr/bin/env python3
"""Installe une action Krusader et son raccourci local réversible, sans modifier F5/F6."""
import argparse
import json
from pathlib import Path
import shlex
import sys
from xml.dom import minidom
from xml.parsers.expat import ExpatError
from dolphin_shortcut import atomic_write
from xdg_paths import data_home

ACTION = 'gad_paste'


def receipt_path():
    """Le reçu contient les propriétés antérieures, pas une configuration à écraser."""
    return data_home() / 'gad/krusader-paste.json'


def document(path, initial):
    """Refuse un XML invalide avant toute écriture et conserve ses commentaires."""
    return minidom.parseString(path.read_bytes() if path.exists() else initial)


def signature(value):
    """Normalise les touches pour détecter les raccourcis concurrents."""
    return frozenset(value.split(',')[0].replace(' ', '').lower().split('+'))


def configure(enabled, binary):
    """Planifie tous les changements avant écriture ; restaure seulement les valeurs détenues."""
    data = data_home()
    receipt = receipt_path()
    state = json.loads(receipt.read_text()) if receipt.exists() else {'version': 1, 'enabled': False, 'files': {}}
    if state.get('version') != 1 or not isinstance(state.get('files'), dict):
        raise ValueError('Invalid Krusader shortcut receipt.')
    if not enabled and not state['enabled']:
        return
    user = data / 'krusader/useractions.xml'
    candidates = [data / name / 'krusader/krusaderui.rc' for name in ('kxmlgui5', 'kxmlgui6')]
    allowed = {str(p) for p in [user, *candidates]}
    if set(state['files']) - allowed:
        raise ValueError('Invalid Krusader configuration path in receipt.')
    paths = ([user, *([p for p in candidates if p.exists()] or candidates[:1])]
             if enabled else [Path(p) for p in state['files']])
    plans = []
    # Le chemin du programme est constant et échappé ; Krusader échappe lui-même %aPath%.
    if not binary.is_absolute() or '%' in str(binary):
        raise ValueError('Gad executable must have an absolute path without %.')
    for path in paths:
        original = path.read_bytes() if path.exists() else None
        is_user = path == user
        doc = document(path, '<KrusaderUserActions/>' if is_user else '<gui name="krusader"/>')
        if doc.documentElement.tagName != ('KrusaderUserActions' if is_user else 'gui'):
            raise ValueError('Unexpected Krusader XML root.')
        if is_user:
            parent = doc.documentElement
            targets = {ACTION: None}
        else:
            groups = [g for g in doc.getElementsByTagName('ActionProperties') if g.getAttribute('scheme') in ('', 'Default')]
            if len(groups) > 1:
                raise ValueError('Multiple default Krusader shortcut schemes.')
            parent = groups[0] if groups else doc.createElement('ActionProperties')
            if not groups:
                parent.setAttribute('scheme', 'Default');doc.documentElement.appendChild(parent)
            targets = {'std_paste': '', 'edit_paste': '', ACTION: 'Ctrl+V'}
        tag = 'action' if is_user else 'Action'
        nodes = {}
        for node in parent.getElementsByTagName(tag):
            name = node.getAttribute('name')
            if name in nodes:
                raise ValueError('Duplicate Krusader actions.')
            nodes[name] = node
            key = node.getAttribute('shortcut') if not is_user else ''.join(c.firstChild.data for c in node.getElementsByTagName('defaultshortcut') if c.firstChild)
            if enabled and name not in targets and any(signature(k) == signature('Ctrl+V') for k in key.split(';')):
                raise ValueError('Ctrl+V is already assigned to another Krusader action: ' + name)
        saved = state['files'].setdefault(str(path), {})
        for name, value in targets.items():
            node = nodes.get(name)
            if enabled:
                if name not in saved:
                    if is_user and node is not None:
                        raise ValueError('An existing gad_paste action is preserved; rename it first.')
                    saved[name] = {'original': node.toxml() if node else None}
                # Une interruption peut laisser l’ancienne valeur après publication du reçu.
                # Seuls les états exacts connus (avant/après) peuvent être repris.
                elif node is not None and node.toxml() not in (saved[name]['installed'], saved[name]['original']):
                    raise ValueError('Krusader Gad settings were modified; configuration preserved.')
                if is_user:
                    if node is not None:parent.removeChild(node)
                    node = doc.createElement('action');node.setAttribute('name', ACTION);parent.appendChild(node)
                    for key, text in [('title', 'Paste with Gad'), ('icon', 'edit-paste'), ('command', shlex.quote(str(binary)) + ' --paste %aPath%'), ('defaultshortcut', 'Ctrl+V')]:
                        child = doc.createElement(key);child.appendChild(doc.createTextNode(text));node.appendChild(child)
                    node.getElementsByTagName('command')[0].setAttribute('accept', 'url')
                else:
                    if node is None:
                        node = doc.createElement('Action');node.setAttribute('name', name);parent.appendChild(node)
                    # Seul Ctrl+V est retiré : les autres raccourcis explicites restent inchangés.
                    if name in ('std_paste', 'edit_paste'):
                        old = node.getAttribute('shortcut')
                        value = '; '.join(k.strip() for k in old.split(';') if k.strip() and signature(k) != signature('Ctrl+V'))
                    node.setAttribute('shortcut', value)
                saved[name]['installed'] = node.toxml()
            elif node is not None and node.toxml() == saved[name]['installed']:
                previous = saved[name]['original']
                if previous is None:parent.removeChild(node)
                else:parent.replaceChild(doc.importNode(minidom.parseString(previous).documentElement, True), node)
        plans.append((path, original, doc.toxml(encoding='UTF-8')))
    # Le reçu précède les XML pour rendre une interruption récupérable.
    old_receipt = receipt.read_bytes() if receipt.exists() else None
    state['enabled'] = enabled
    if enabled:atomic_write(receipt, json.dumps(state).encode(), private=True)
    written = []
    try:
        for path, original, updated in plans:
            atomic_write(path, updated);written.append((path, original))
        if not enabled:
            atomic_write(receipt, json.dumps({'version': 1, 'enabled': False, 'files': {}}).encode(), private=True)
    except OSError:
        for path, original in reversed(written):
            if original is None:path.unlink(missing_ok=True)
            else:atomic_write(path, original)
        if old_receipt is None:receipt.unlink(missing_ok=True)
        else:atomic_write(receipt, old_receipt, private=True)
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--enable-ctrl-v', action='store_true')
    group.add_argument('--disable-ctrl-v', action='store_true')
    parser.add_argument('--binary', type=Path, required=True)
    args = parser.parse_args()
    try:
        configure(args.enable_ctrl_v, args.binary)
    except (OSError, ValueError, KeyError, ExpatError) as error:
        print(str(error), file=sys.stderr);sys.exit(1)
