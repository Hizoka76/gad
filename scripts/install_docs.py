"""Installe les exports déjà générés, sans exiger les outils de rendu chez l’utilisateur."""
import json
from pathlib import Path
from doc_catalog import runtime_catalog


def install(destination, source=None):
    """Remplace les fichiers connus par renommage et conserve les autres documents de l’utilisateur."""
    source=source or Path(__file__).resolve().parent.parent/'docs'
    catalog=runtime_catalog();copied=0
    for guide in catalog['guides']:
        for kind in ('html','pdf'):
            relative=guide[kind];original=source/relative
            if not original.is_file():continue
            target=destination/relative;target.parent.mkdir(parents=True,exist_ok=True)
            data=original.read_bytes()
            if not target.exists() or target.read_bytes()!=data:
                staged=target.with_name('.'+target.name+'.install');staged.write_bytes(data);staged.replace(target)
            copied+=1
    destination.mkdir(parents=True,exist_ok=True)
    staged=destination/'.catalog.json.install';staged.write_text(json.dumps(catalog,ensure_ascii=False,indent=2)+'\n');staged.replace(destination/'catalog.json')
    print(f'Documentation installée : {copied} fichiers HTML/PDF')
