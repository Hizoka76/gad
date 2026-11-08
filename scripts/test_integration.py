#!/usr/bin/env python3
"""Teste le vrai binaire, la socket Qt et le journal dans un espace temporaire sans toucher au presse-papiers du bureau."""
import json
import hashlib
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parent.parent
binary = Path(os.environ.get("GAD_TEST_BINARY", str(root / "target/debug/gad")))
# Isoler à la fois données, configuration, socket et journal pour ne jamais atteindre la session Gad réelle.
with tempfile.TemporaryDirectory(prefix="gad-integration-") as tmp:
    base = Path(tmp)
    runtime = base / "runtime"
    runtime.mkdir(mode=0o700)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(base / "config"), XDG_DATA_HOME=str(base / "data"))
    # Les préférences proviennent uniquement de la configuration Gad.
    settings = base / "config/Gad/Gad.conf"
    settings.parent.mkdir(parents=True)
    settings.write_text("[General]\nverifyMd5=true\nkeepHistory=true\nverifySpace=true\n")
    # Les noms difficiles et liens rendent les assertions sensibles aux mauvaises conversions de chemins.
    source = base / 'source avec espaces et "guillemets"'
    source.mkdir()
    (source / "été\n.txt").write_bytes(bytes(range(256)) * 8192)
    (source / "vide").mkdir()
    (source / "lien").symlink_to("été\n.txt")
    dest = base / "destination"
    dest.mkdir()
    log = open(base / "gui.log", "w+")
    # Le démarrage discret doit créer un serveur utilisable avant toute ouverture de fenêtre.
    process = subprocess.Popen([binary, "--background"], env=env, stdout=log, stderr=log)

    def request(value):
        """Envoie une ligne JSON à l’instance isolée et attend sa réponse complète."""
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(5)
            client.connect(str(runtime / "gad.sock"))
            client.sendall(json.dumps(value).encode() + b"\n")
            reply = b""
            while b"\n" not in reply:
                reply += client.recv(65536)
            return json.loads(reply)

    def wait(predicate):
        """Sonde l’état avec une échéance et détecte aussi une fermeture inattendue du processus."""
        for _ in range(200):
            try:
                state = request({"command": "status"})
                if predicate(state):
                    return state
            except (FileNotFoundError, ConnectionRefusedError):
                pass
            if process.poll() is not None:
                raise AssertionError("La fenêtre s’est arrêtée")
            time.sleep(0.05)
        raise AssertionError("Délai dépassé")

    try:
        initial=wait(lambda s: "jobs" in s)
        # La vraie frontière Qt/Rust doit produire un plan sans fichier ni lot.
        token=request({"command":"simulation_begin"})
        preview=request({"command":"simulate","id":token["id"],"sources":[str(source)],"destination":str(dest)})
        assert preview.get("ok") and preview["rows"]
        assert not list(dest.iterdir()) and (source/"été\n.txt").is_file()
        assert request({"command":"status"})["jobs"]==initial["jobs"]
        assert any(row["kind"]=="link" for row in preview["rows"])

        # Le lien de progression et un second démarrage ciblent la même instance sans ajouter de lot.
        for argument in ("gad://show", "--background"):
            client = subprocess.run([binary, argument], env=env, capture_output=True, text=True, timeout=10)
            assert client.returncode == 0 and json.loads(client.stdout)["ok"], client.stderr
        # Les noms Unix non UTF-8 ne doivent pas être confondus avec un autre fichier Unicode existant.
        invalid_source=os.fsencode(base)+b"/bad-\xff"
        with open(invalid_source,"wb") as output: output.write(b"invalid-byte-name")
        (base / "bad-\ufffd").write_bytes(b"different-unicode-file")
        rejected=subprocess.run([os.fsencode(binary),b"--copy",invalid_source,b"--to",os.fsencode(dest)],env=env,capture_output=True,timeout=10)
        assert rejected.returncode!=0,"Un argument non UTF-8 a été accepté et peut désigner un autre fichier"
        assert not request({"command":"status"})["jobs"]
        assert initial["verify_md5"] and initial["keep_history"] and initial["verify_space"]
        assert (base / "data/gad/state.json").exists()
        assert (base / "config/Gad/Gad.conf").exists()
        request({"command": "configure", "verify_md5": False})
        # Soumettre deux copies pendant la pause pour contrôler leur ordre et les conflits qui suivent.
        request({"command": "pause", "paused": True})
        # Deux sources distinctes portant le même nom produisent un vrai conflit, pas un doublon.
        import shutil
        second_source = base / "other" / source.name
        shutil.copytree(source, second_source, symlinks=True)
        for queued_source in (source, second_source):
            result = subprocess.run([binary, "--copy", queued_source, "--to", dest], env=env, capture_output=True, text=True, timeout=10)
            assert result.returncode == 0, result.stderr
            assert "id" in json.loads(result.stdout)
        state = wait(lambda s: len(s["jobs"]) == 2 and all(j["state"] == "waiting" for j in s["jobs"]))
        # Le protocole de priorité réordonne la file sous pause, puis retrouve son ordre initial.
        reordered=request({"command":"reorder_queue","ids":[2],"action":"next"})
        assert reordered["queue_order"]==[2,1]
        assert request({"command":"status"})["queue_order"]==[2,1]
        request({"command":"reorder_queue","ids":[2],"action":"down"})
        assert request({"command":"status"})["queue_order"]==[1,2]
        # Le dépôt d’une branche traverse aussi le vrai routeur Qt, puis la réponse différentielle.
        moved=request({"command":"reorder_rows","rows":[{"job":2,"index":0}],"target":{"job":1,"index":0},"action":"before"})
        assert moved["row_order"][0]=={"job":2,"index":0}
        request({"command":"reorder_rows","rows":[{"job":1,"index":0}],"target":{"job":2,"index":0},"action":"before"})
        assert request({"command":"status"})["row_order"][0]=={"job":1,"index":0}
        assert not (dest / source.name).exists(), "La pause doit empêcher la première copie"
        request({"command": "pause", "paused": False})
        state = wait(lambda s: s["jobs"][0]["state"] == "done" and any(e["state"] == "conflict" for e in s["jobs"][1]["entries"]))
        request({"command": "resolve", "id": state["jobs"][1]["id"], "action": "rename", "all": True})
        wait(lambda s: all(j["state"] == "done" for j in s["jobs"]))
        # Vérifier les contenus et les noms publiés, pas seulement les états déclarés par le moteur.
        output = dest / source.name
        assert (output / "été\n.txt").read_bytes() == (source / "été\n.txt").read_bytes()
        assert (output / "été\n (copie 1).txt").read_bytes() == (source / "été\n.txt").read_bytes()
        assert (output / "lien").readlink() == Path("été\n.txt")
        for job in request({"command":"status"})["jobs"]:
            for entry in job["entries"]:
                staged=(entry.get("undo") or {}).get("staging")
                if staged:
                    partial=Path(staged)
                    assert not partial.exists() and not partial.is_symlink(),partial
        result = subprocess.run([binary, "--status"], env=env, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0 and len(json.loads(result.stdout)["jobs"]) == 2
        # Le mode déplacement avec MD5 doit retirer la source après vérification du résultat.
        moved = base / "à déplacer"
        moved.mkdir()
        (moved / "document").write_bytes(b"original")
        result = subprocess.run([binary, "--move", moved, "--to", dest, "--md5"], env=env, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stderr
        state = wait(lambda s: len(s["jobs"]) == 3 and s["jobs"][2]["state"] == "done")
        assert state["jobs"][2]["mode"] == "move"
        assert state["jobs"][2]["verify_md5"]
        hashes = [e for e in state["jobs"][2]["entries"] if e["kind"] == "file"]
        assert hashes[0]["md5_source"] == hashes[0]["md5_destination"] == hashlib.md5(b"original").hexdigest()
        # Le réglage MD5 par défaut doit être capturé même sans option CLI explicite.
        request({"command": "configure", "verify_md5": True})
        source_md5 = base / "md5-source"
        source_md5.write_bytes(bytes(range(256)) * 8192)
        result = subprocess.run([binary, "--copy", source_md5, "--to", dest], env=env, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0
        state = wait(lambda s: len(s["jobs"]) == 4 and s["jobs"][3]["state"] == "done")
        entry = state["jobs"][3]["entries"][0]
        expected = hashlib.md5(source_md5.read_bytes()).hexdigest()
        assert state["jobs"][3]["verify_md5"] and entry["md5_source"] == entry["md5_destination"] == expected
        assert not moved.exists() and (dest / moved.name / "document").read_bytes() == b"original"
        # Une vraie réouverture doit relire les opérations terminées et leurs journaux de restauration.
        request({"command": "shutdown"})
        process.terminate(); process.wait(timeout=5)
        process = subprocess.Popen([binary], env=env, stdout=log, stderr=log)
        state = wait(lambda s: len(s.get("jobs", [])) == 4)
        assert all(j["state"] == "done" and j["verify_space"] for j in state["jobs"])
        assert state["verify_space"]
        request({"command": "pause", "paused": True})
        # La restauration multiple doit rester disponible pendant la pause et vider les résultats de test.
        request({"command": "undo_many", "ids": [1, 2, 3, 4]})
        state = wait(lambda s: all(j["state"] in ("undone", "undo_error") for j in s["jobs"]))
        assert all(j["state"] == "undone" for j in state["jobs"]), state
        assert moved.is_dir() and (moved / "document").read_bytes() == b"original"
        assert not list(dest.iterdir())
        assert source_md5.exists() and source.is_dir()
        # L’option CLI compare les attributs sans transformer une copie réussie en erreur de contenu.
        prop_source=base / "proprietes.bin";prop_source.write_bytes(b"contenu")
        os.setxattr(prop_source,"user.gad-check",b"valeur-privee")
        request({"command":"pause","paused":False})
        result=subprocess.run([binary,"--copy",prop_source,"--to",dest,"--properties"],env=env,capture_output=True,text=True,timeout=10)
        assert result.returncode==0,result.stderr
        prop_id=json.loads(result.stdout)["id"]
        prop_state=wait(lambda s:any(j["id"]==prop_id and j["state"]=="done" for j in s["jobs"]))
        prop_job=next(j for j in prop_state["jobs"] if j["id"]==prop_id)
        assert prop_job["verify_properties"] and prop_job["entries"][0]["properties"]["status"]=="different"
        assert "valeur-privee" not in json.dumps(prop_job)
        assert (dest / prop_source.name).read_bytes()==b"contenu"
        request({"command":"undo_many","ids":[prop_id]})
        wait(lambda s:next(j for j in s["jobs"] if j["id"]==prop_id)["state"]=="undone")
        # Une file inactive attend le bouton ; un ajout externe démarre toute la file.
        manual=base / "manual.txt";manual.write_bytes(b"manual preparation")
        manual_dest=base / "manual-destination";manual_dest.mkdir()
        prepared=request({"command":"enqueue","prepare_only":True,"sources":[str(manual)],"destination":str(manual_dest)})
        manual_id=prepared["id"]
        wait(lambda s:any(j["id"]==manual_id and j["state"]=="waiting" for j in s["jobs"]))
        request({"command":"pause","paused":False})
        automatic=base / "automatic.txt";automatic.write_bytes(b"automatic transfer")
        added=subprocess.run([binary,"--copy",automatic,"--to",dest],env=env,capture_output=True,text=True,timeout=10)
        assert added.returncode==0,added.stderr
        automatic_id=json.loads(added.stdout)["id"]
        wait(lambda s:any(j["id"]==automatic_id and j["state"]=="done" for j in s["jobs"]))
        wait(lambda s:any(j["id"]==manual_id and j["state"]=="done" for j in s["jobs"]))
        manual_after=base / "manual-after.txt";manual_after.write_bytes(b"next preparation")
        after=request({"command":"enqueue","prepare_only":True,"sources":[str(manual_after)],"destination":str(manual_dest)})["id"]
        wait(lambda s:any(j["id"]==after and j["state"]=="waiting" for j in s["jobs"]))
        assert not (manual_dest/manual_after.name).exists()
        assert request({"command":"pause","paused":False})["ok"]
        wait(lambda s:any(j["id"]==after and j["state"]=="done" for j in s["jobs"]))
        assert (manual_dest/manual.name).read_bytes()==b"manual preparation"
        # La fermeture sans conservation d’historique libère les journaux des lots terminés.
        request({"command": "configure", "keep_history": False})
        request({"command": "shutdown"})
        assert not request({"command": "status"})["jobs"]
        process.terminate(); process.wait(timeout=5)
        # Sans instance Gad, --status échoue et ne démarre aucun moteur.
        result=subprocess.run([binary,"--status"],env=env,capture_output=True,text=True,timeout=10)
        assert result.returncode!=0 and "Gad n’est pas lancé" in result.stderr
        # Une variable XDG vide ou relative ne doit pas placer le journal ni le raccourci dans le dossier courant.
        for index,xdg_value in enumerate(("","relative-data")):
            case=base / f"xdg-{index}";case.mkdir();home=case / "home";home.mkdir();work=case / "work";work.mkdir()
            case_runtime=case / "runtime";case_runtime.mkdir(mode=0o700)
            case_env=dict(env,HOME=str(home),XDG_DATA_HOME=xdg_value,XDG_CONFIG_HOME=str(case / "config"),XDG_RUNTIME_DIR=str(case_runtime))
            with open(case / "gui.log","w+") as case_log:
                child=subprocess.Popen([binary,"--background"],env=case_env,cwd=work,stdout=case_log,stderr=case_log)
                try:
                    for _ in range(100):
                        probe=subprocess.run([binary,"--status"],env=case_env,cwd=work,capture_output=True,text=True,timeout=10)
                        if probe.returncode==0:break
                        if child.poll() is not None:raise AssertionError("Échec du lancement XDG isolé")
                        time.sleep(.02)
                    else:raise AssertionError("Lancement XDG isolé trop long")
                    assert (home / ".local/share/gad/state.json").exists(),"Le journal doit utiliser le dossier de données par défaut"
                    subprocess.run(["python3",root / "scripts/dolphin_shortcut.py"],env=case_env,cwd=work,capture_output=True,check=True,timeout=10)
                    assert (home / ".local/share/kxmlgui5/dolphin/dolphinui.rc").exists(),"Le raccourci doit suivre le même choix XDG que le moteur"
                    assert not (work / "gad").exists() and not (work / "relative-data").exists() and not (work / "kxmlgui5").exists()
                finally:
                    child.terminate();child.wait(timeout=5)
        print("OK : préférences Gad, instance unique IPC, historique, MD5 et remise en état multiple.")
    # Même après une assertion en échec, arrêter le processus enfant et fermer son fichier de log.
    finally:
        process.terminate()
        process.wait(timeout=5)
        log.close()

# Un second environnement privé couvre l’arrêt brutal et la reprise réelle du moteur.
subprocess.run([sys.executable, root / "scripts/test_resume_integration.py"], check=True)
