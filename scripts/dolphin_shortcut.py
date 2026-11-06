#!/usr/bin/env python3
"""Configure uniquement le raccourci natif de l’action Gad dans les réglages XML de Dolphin."""
from pathlib import Path
from xdg_paths import data_home
import shutil
import argparse
import json
import os
from xml.dom import minidom

ACTION = "servicemenu_gad-paste.desktop::GadPaste"
CURRENT_ACTION = "servicemenu_gad-paste-current.desktop::GadPasteCurrent"
SHORTCUT = ""
REMOVED_SHORTCUT = "Ctrl+Shift+V"
PREVIOUS_PASTE_SHORTCUT = "Ctrl+V; Ctrl+Shift+V"
PASTE_SHORTCUT = "Ctrl+V"
NATIVE_ACTION = "edit_paste"
NATIVE_SHORTCUT = "Shift+Ins"
LEGACY_NATIVE_SHORTCUT = "Ctrl+Alt+V; Shift+Ins"


def receipt_path():
    """Conserve uniquement les anciennes propriétés de raccourci, jamais une copie globale à restaurer aveuglément."""
    return data_home() / "gad/dolphin-paste.json"


def receipt():
    """Un état illisible est conservé et provoque un refus plutôt qu’une restauration supposée."""
    path=receipt_path()
    if not path.exists(): return {"version":2,"files":{}}
    value=json.loads(path.read_text())
    if not isinstance(value,dict) or value.get("version") not in (1,2) or not isinstance(value.get("files"),dict):
        raise ValueError("État du raccourci Ctrl+V illisible ; configuration conservée")
    allowed={str(data_home()/name/"dolphin/dolphinui.rc") for name in ("kxmlgui5","kxmlgui6")}
    for path,actions in value["files"].items():
        if path not in allowed or not isinstance(actions,dict) or set(actions)!=({ACTION,NATIVE_ACTION} if value["version"]==1 else {ACTION,CURRENT_ACTION,NATIVE_ACTION}):
            raise ValueError("Reçu de raccourcis non reconnu ; configuration conservée")
        for old in actions.values():
            if not isinstance(old,dict) or not isinstance(old.get("present"),bool) or not isinstance(old.get("value"),str):
                raise ValueError("Ancien raccourci illisible ; configuration conservée")
    return value


def xml_paths():
    """Les deux emplacements sont traités lorsqu’ils existent ; sinon Dolphin utilise kxmlgui5."""
    data=data_home()
    paths=[data / name / "dolphin/dolphinui.rc" for name in ("kxmlgui5","kxmlgui6")]
    return [path for path in paths if path.exists()] or paths[:1]


def atomic_write(path,contents,private=False):
    """Écrit et synchronise avant remplacement ; conserve les permissions du XML existant."""
    path.parent.mkdir(parents=True,exist_ok=True)
    temporary=path.with_name(path.name+".gad-paste.tmp")
    flags=os.O_WRONLY|os.O_CREAT|os.O_TRUNC|os.O_NOFOLLOW
    descriptor=os.open(temporary,flags,0o600)
    try:
        with os.fdopen(descriptor,"wb") as output:
            if private:os.fchmod(output.fileno(),0o600)
            output.write(contents);output.flush();os.fsync(output.fileno())
        if path.exists() and not private:shutil.copymode(path,temporary)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def configure_paste(enabled):
    """Attribue Ctrl+V à Gad dans Dolphin, ou restaure seulement les propriétés encore détenues par Gad."""
    state=receipt()
    # Ancien mode : l’action de contexte utilisait la sélection de Dolphin, parfois périmée après navigation.
    if enabled and state["version"]==1 and state["files"]:
        configure_paste(False)
        state=receipt()
    if enabled:state["version"]=2
    previous=receipt_path().read_bytes() if receipt_path().exists() else None;plans=[]
    if not enabled and not state["files"]:return
    paths=xml_paths() if enabled else [Path(path) for path in state["files"]]
    for path in paths:
        original=path.read_bytes() if path.exists() else None
        if not enabled and original is None:continue
        document=minidom.parseString(original) if original else minidom.parseString('<gui name="dolphin"><ActionProperties scheme="Default"/></gui>')
        groups=[group for group in document.getElementsByTagName("ActionProperties") if group.getAttribute("scheme") in ("","Default")]
        if len(groups)>1:raise ValueError("Plusieurs schémas Dolphin par défaut : configuration conservée")
        if groups:group=groups[0]
        else:
            group=document.createElement("ActionProperties");group.setAttribute("scheme","Default");document.documentElement.appendChild(group)
        by_name={}
        for action in group.getElementsByTagName("Action"):
            name=action.getAttribute("name")
            if name in by_name:raise ValueError("Actions Dolphin dupliquées : configuration conservée")
            by_name[name]=action
        saved=state["files"].get(str(path),{})
        installed=({ACTION:PASTE_SHORTCUT,NATIVE_ACTION:NATIVE_SHORTCUT} if state["version"]==1
                   else {ACTION:"",CURRENT_ACTION:PASTE_SHORTCUT,NATIVE_ACTION:NATIVE_SHORTCUT})
        if enabled:
            # Ne pas prendre une touche déjà attribuée à une autre action personnalisée.
            def signature(sequence):
                """Comparer aussi le début d’un raccourci composé, indépendamment de l’ordre des modificateurs."""
                aliases={"control":"ctrl","insert":"ins"}
                return frozenset(aliases.get(key,key) for key in sequence.split(",")[0].replace(" ","").casefold().split("+"))
            reserved={signature(key) for key in ("Ctrl+V","Shift+Ins")}
            for name,action in by_name.items():
                if name not in installed and reserved.intersection(signature(part) for part in action.getAttribute("shortcut").split(";")):
                    raise ValueError(f"Raccourci déjà utilisé par {name} ; aucun réglage n’a été modifié")
            for name,value in installed.items():
                action=by_name.get(name)
                if action is None:
                    action=document.createElement("Action");action.setAttribute("name",name);group.appendChild(action)
                # Un second enable ne remplace pas la valeur initiale par notre propre raccourci.
                legacy_native=name==NATIVE_ACTION and action.getAttribute("shortcut")==LEGACY_NATIVE_SHORTCUT
                previous_gad=name in (ACTION,CURRENT_ACTION) and action.getAttribute("shortcut") in (REMOVED_SHORTCUT,PREVIOUS_PASTE_SHORTCUT)
                if name not in saved or (action.getAttribute("shortcut")!=value and not legacy_native and not previous_gad):
                    saved[name]={"present":action.hasAttribute("shortcut"),"value":action.getAttribute("shortcut")}
                    if name==ACTION and not saved[name]["present"]:saved[name]={"present":True,"value":SHORTCUT}
                action.setAttribute("shortcut",value)
            state["files"][str(path)]=saved
        else:
            for name,old in saved.items():
                action=by_name.get(name)
                if action is None:continue
                owned=action.getAttribute("shortcut")==installed.get(name)
                owned |= name in (ACTION,CURRENT_ACTION) and action.getAttribute("shortcut") in (REMOVED_SHORTCUT,PREVIOUS_PASTE_SHORTCUT)
                owned |= name==NATIVE_ACTION and action.getAttribute("shortcut")==LEGACY_NATIVE_SHORTCUT
                if not owned:continue
                if name in (ACTION,CURRENT_ACTION) and old["value"] in (REMOVED_SHORTCUT,PREVIOUS_PASTE_SHORTCUT):action.setAttribute("shortcut","")
                elif old["present"]:action.setAttribute("shortcut",old["value"])
                else:action.removeAttribute("shortcut")
                if action.attributes.length==1:group.removeChild(action)
        plans.append((path,original,document.toxml(encoding="UTF-8")))
    if not enabled:state["files"]={}
    # Le reçu précède les changements XML pour permettre une restauration après interruption.
    state_file=receipt_path();had_receipt=state_file.exists();written=[]
    if enabled:atomic_write(state_file,json.dumps(state,ensure_ascii=False,indent=2).encode(),private=True)
    try:
        for path,original,updated in plans:
            if original==updated:continue
            if original and enabled:
                backup=path.with_name(path.name+".gad-paste.bak")
                if not backup.exists():shutil.copy2(path,backup)
            atomic_write(path,updated);written.append((path,original))
        # Pendant une désactivation interrompue, garder l’ancien reçu jusqu’à la restauration de tous les XML.
        if not enabled:atomic_write(state_file,json.dumps(state,ensure_ascii=False,indent=2).encode(),private=True)
    except OSError:
        for path,original in reversed(written):
            if original is None:path.unlink(missing_ok=True)
            else:atomic_write(path,original)
        if had_receipt:atomic_write(state_file,previous,private=True)
        else:state_file.unlink(missing_ok=True)
        raise
    print("Dolphin : Ctrl+V avec Gad activé" if enabled else "Dolphin : raccourcis de collage restaurés")
    print("Fermez puis rouvrez Dolphin pour appliquer le changement. Le collage de texte reste natif.")


def configure(remove=False):
    """Retire le raccourci secondaire et préserve les autres actions.

    Une suppression ne retire un raccourci que s’il correspond encore à celui installé.
    """
    if remove:configure_paste(False)
    owned=receipt()["files"]
    if owned and not remove:
        configure_paste(True)
        owned=receipt()["files"]
    data = data_home()
    # Dolphin peut conserver ses réglages sous kxmlgui5 même lorsqu’il utilise Qt 6.
    paths = [data / name / "dolphin/dolphinui.rc" for name in ("kxmlgui5", "kxmlgui6")]
    paths = [path for path in paths if path.exists()] or paths[:1]
    for path in paths:
        if not path.exists() and remove:
            continue
        original = path.read_bytes() if path.exists() else None
        document = minidom.parseString(original) if original else minidom.parseString('<gui name="dolphin"><ActionProperties scheme="Default"/></gui>')
        root = document.documentElement
        # Ne modifier que le schéma de raccourcis par défaut, pas les schémas personnels supplémentaires.
        groups = [g for g in root.getElementsByTagName("ActionProperties") if g.getAttribute("scheme") in ("", "Default")]
        if not groups:
            if remove:
                continue
            group = document.createElement("ActionProperties")
            group.setAttribute("scheme", "Default")
            root.appendChild(group)
        else:
            group = groups[0]
        actions = [a for a in group.getElementsByTagName("Action") if a.getAttribute("name") == ACTION]
        # La désinstallation ne doit pas effacer un nouveau raccourci choisi depuis par l’utilisateur.
        if remove:
            for action in actions:
                if action.getAttribute("shortcut") in (SHORTCUT,REMOVED_SHORTCUT):
                    action.removeAttribute("shortcut")
                    if action.attributes.length == 1:
                        group.removeChild(action)
        else:
            action = actions[0] if actions else document.createElement("Action")
            if not actions:
                action.setAttribute("name", ACTION)
                group.appendChild(document.createTextNode("\n  "))
                group.appendChild(action)
                group.appendChild(document.createTextNode("\n "))
            if str(path) not in owned:action.setAttribute("shortcut", SHORTCUT)
        updated = document.toxml(encoding="UTF-8")
        # Éviter une écriture identique empêche les rechargements inutiles de raccourcis dans Dolphin.
        if updated == original:
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        if original and not remove:
            backup = path.with_name(path.name + ".gad.bak")
            if not backup.exists():
                shutil.copy2(path, backup)
        # Le remplacement par renommage évite un fichier XML partiellement écrit.
        temporary = path.with_name(path.name + ".gad.tmp")
        temporary.write_bytes(updated)
        if path.exists():
            shutil.copymode(path, temporary)
        temporary.replace(path)
        print(f"Raccourci Gad {'retiré' if remove else 'actualisé'} : {path}")


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description="Raccourcis Dolphin de Gad, sans interception globale du clavier")
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument("--enable-ctrl-v","--enable",action="store_true")
    mode.add_argument("--disable-ctrl-v","--disable",action="store_true")
    mode.add_argument("--status",action="store_true")
    args=parser.parse_args()
    try:
        if args.enable_ctrl_v:configure_paste(True)
        elif args.disable_ctrl_v:configure_paste(False)
        elif args.status:print(json.dumps(receipt(),ensure_ascii=False,indent=2))
        else:configure()
    except (OSError,ValueError) as error:
        parser.exit(1,str(error)+"\n")
