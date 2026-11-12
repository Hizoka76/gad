#!/usr/bin/env python3
"""Essai optionnel sur un vrai serveur FTP local et un montage KIO Fuse privés."""
import importlib.util
import json
import logging
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from private_test_session import run as run_private_session

ROOT = Path(__file__).resolve().parent.parent


def exercise():
    """Tous les processus, données et connexions appartiennent à ce test ; le serveur écoute seulement localhost."""
    from pyftpdlib.authorizers import DummyAuthorizer
    from pyftpdlib.handlers import FTPHandler
    from pyftpdlib.servers import FTPServer

    fuse_binary = next((path for path in (shutil.which("kio-fuse"), "/usr/lib/x86_64-linux-gnu/libexec/kio-fuse", "/usr/libexec/kio-fuse") if path and Path(path).exists()), None)
    if not fuse_binary:
        raise RuntimeError("Installer kio-fuse pour cet essai optionnel")
    with tempfile.TemporaryDirectory(prefix="gad-remote-") as tmp:
        base = Path(tmp)
        for name in ("remote", "mount", "home"):
            (base / name).mkdir()
        (base / "runtime").mkdir(mode=0o700)
        source = base / "source.bin"
        data = bytes(range(256)) * 1024
        source.write_bytes(data)
        # Créer le conflit avant le montage pour que KIO en observe immédiatement la bonne version.
        existing = base / "existing.bin"
        existing.write_bytes(b"nouveau contenu")
        (base / "remote/existing.bin").write_bytes(b"ancien contenu")
        authorizer = DummyAuthorizer()
        authorizer.add_user("gadtest", "local-test-only", str(base / "remote"), perm="elradfmwMT")

        class Handler(FTPHandler):
            """L’autorisation donne accès uniquement au répertoire temporaire exporté."""

        Handler.authorizer = authorizer
        logging.getLogger("pyftpdlib").disabled = True
        server = FTPServer(("127.0.0.1", 0), Handler)
        port = server.socket.getsockname()[1]
        worker = threading.Thread(target=server.serve_forever, kwargs={"timeout": .05}, daemon=True)
        worker.start()
        address = f"ftp://gadtest:local-test-only@127.0.0.1:{port}/"
        env = dict(os.environ, HOME=str(base / "home"), QT_QPA_PLATFORM="offscreen",
                   XDG_RUNTIME_DIR=str(base / "runtime"), XDG_CONFIG_HOME=str(base / "config"),
                   XDG_DATA_HOME=str(base / "data"), XDG_CACHE_HOME=str(base / "cache"))
        env["DBUS_SYSTEM_BUS_ADDRESS"] = env["DBUS_SESSION_BUS_ADDRESS"]
        settings = base / 'config/Gad/Gad.conf'
        settings.parent.mkdir(parents=True)
        settings.write_text('[General]\nprogressMessage=false\n')
        binary = Path(os.environ.get("GAD_TEST_BINARY", ROOT / "target/debug/gad")).resolve()
        with (base / "gui.log").open("w+") as log:
            fuse = subprocess.Popen([fuse_binary, "-f", str(base / "mount")], env=env, stdout=log, stderr=log)
            gad = None

            def request(value):
                """Interroge uniquement la socket de l’instance privée."""
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(10)
                    client.connect(str(base / "runtime/gad.sock"))
                    client.sendall(json.dumps(value).encode() + b"\n")
                    reply = b""
                    while b"\n" not in reply:
                        chunk = client.recv(65536)
                        if not chunk:
                            raise ConnectionError("Réponse Gad interrompue")
                        reply += chunk
                    return json.loads(reply)

            def wait(predicate):
                """Détecte les délais et les processus morts sans laisser tourner le serveur après un échec."""
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    if gad.poll() is not None or fuse.poll() is not None:
                        raise AssertionError("Une instance de test s’est arrêtée")
                    try:
                        state = request({"command": "status"})
                        if predicate(state):
                            return state
                    except (FileNotFoundError, ConnectionRefusedError):
                        pass
                    time.sleep(.02)
                raise AssertionError("Délai de transfert FTP dépassé")

            def job_state(identifier, expected):
                """Une erreur échoue immédiatement et indique le détail du fichier concerné."""
                def predicate(state):
                    job = next((job for job in state["jobs"] if job["id"] == identifier), None)
                    if job and job["state"] in ("error", "undo_error"):
                        raise AssertionError([(entry["state"], entry["error"]) for entry in job["entries"]])
                    return job is not None and job["state"] == expected
                return wait(predicate)

            try:
                gad = subprocess.Popen([binary, "--background"], env=env, stdout=log, stderr=log)
                wait(lambda state: "jobs" in state)
                # La vraie CLI transporte l’URL et ses identifiants, puis le moteur reçoit le montage sans mot de passe.
                copied = subprocess.run([binary, "--copy", source, "--to", address, "--md5"], env=env, capture_output=True, text=True, timeout=10)
                assert copied.returncode == 0 and json.loads(copied.stdout)["preparing"]
                state = job_state(1, "done")
                assert not state["jobs"][0]["resume_enabled"]
                mounted_destination=state["jobs"][0]["destination"]
                assert (base / "remote/source.bin").read_bytes() == data
                request({"command": "undo_many", "ids": [1]})
                job_state(1, "undone")
                assert source.read_bytes() == data and not (base / "remote/source.bin").exists()
                # Déplacer vers FTP puis remettre comme avant doit rendre l’original local avec son contenu.
                moved = subprocess.run([binary, "--move", source, "--to", address, "--md5"], env=env, capture_output=True, text=True, timeout=10)
                assert moved.returncode == 0
                job_state(2, "done")
                assert not source.exists() and (base / "remote/source.bin").read_bytes() == data
                request({"command": "undo_many", "ids": [2]})
                job_state(2, "undone")
                assert source.read_bytes() == data and not (base / "remote/source.bin").exists()
                # Le remplacement conserve une sauvegarde sur le serveur et peut restaurer l’ancienne destination.
                request({"command": "enqueue", "sources": [str(existing)], "destination": address, "background": True})
                wait(lambda state: len(state["jobs"]) == 3 and any(entry["state"] == "conflict" for entry in state["jobs"][2]["entries"]))
                request({"command": "resolve", "id": 3, "index": 0, "action": "replace", "all": False})
                job_state(3, "done")
                assert (base / "remote/existing.bin").read_bytes() == b"nouveau contenu"
                request({"command": "undo_many", "ids": [3]})
                job_state(3, "undone")
                assert (base / "remote/existing.bin").read_bytes() == b"ancien contenu"
                # Les dossiers et leurs enfants suivent les mêmes règles que les fichiers isolés.
                tree=base / "arbre"
                (tree / "sous dossier").mkdir(parents=True)
                (tree / "sous dossier/été #.txt").write_bytes(data)
                request({"command":"enqueue","sources":[str(tree)],"destination":address,"background":True,"verify_md5":True})
                job_state(4,"done")
                assert (base / "remote/arbre/sous dossier/été #.txt").read_bytes()==data
                request({"command":"undo_many","ids":[4]})
                job_state(4,"undone")
                assert not (base / "remote/arbre").exists()
                # Saisir directement le chemin du montage doit désactiver les points durables lui aussi.
                request({"command":"enqueue","sources":[str(source)],"destination":mounted_destination,"background":True})
                mounted_state=job_state(5,"done")
                assert not mounted_state["jobs"][4]["resume_enabled"]
                assert (base / "remote/source.bin").read_bytes()==data
                request({"command":"undo_many","ids":[5]})
                job_state(5,"undone")
                # Un serveur qui ne fournit pas d’ACL/xattr ne doit jamais produire un faux contrôle OK.
                checked=subprocess.run([binary,"--copy",source,"--to",address,"--properties"],env=env,capture_output=True,text=True,timeout=10)
                assert checked.returncode==0
                checked_state=job_state(6,"done")
                report=checked_state["jobs"][5]["entries"][0]["properties"]
                assert report["status"] in ("different","unavailable") and report["unavailable"]
                assert (base / "remote/source.bin").read_bytes()==data
                request({"command":"undo_many","ids":[6]});job_state(6,"undone")
                request({"command":"enqueue","mode":"move","sources":[str(source)],"destination":address,"background":True,"verify_properties":True})
                wait(lambda state:len(state["jobs"])==7 and state["jobs"][6]["state"]=="error")
                assert source.read_bytes()==data and not (base / "remote/source.bin").exists()
                assert "local-test-only" not in (base / "data/gad/state.json").read_text()
                print("OK : FTP réel, CLI, MD5, déplacement, remplacement et remise en état, sans mot de passe persisté.")
            finally:
                if gad is not None and gad.poll() is None:
                    gad.terminate()
                    gad.wait(timeout=10)
                subprocess.run(["fusermount3", "-u", str(base / "mount")], capture_output=True, timeout=10)
                if fuse.poll() is None:
                    fuse.terminate()
                    fuse.wait(timeout=10)
                server.close_all()
                worker.join(timeout=2)


def main():
    """Un bus D-Bus neuf empêche toute activation du KIO Fuse de la session utilisateur."""
    if sys.argv[1:] == ["--private-session"]:
        if importlib.util.find_spec("pyftpdlib") is None:
            raise SystemExit("Ce test optionnel nécessite le module Python pyftpdlib")
        exercise()
    else:
        run_private_session(__file__)


if __name__ == "__main__":
    main()
