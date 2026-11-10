#!/usr/bin/env python3
"""Interrompt un vrai processus isolé puis vérifie sa reprise sur le même temporaire."""
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from journal_reader import read_journal


def main():
    """Le SIGKILL ne cible que notre enfant ; aucun fichier de la session utilisateur n’est utilisé."""
    root = Path(__file__).resolve().parent.parent
    binary = Path(os.environ.get("GAD_TEST_BINARY", str(root / "target/debug/gad")))
    # Éviter /dev/shm, souvent limité à 64 Mio en CI : ce scénario écrit réellement 256 Mio.
    with tempfile.TemporaryDirectory(prefix="resume-", dir=root / "target") as tmp:
        base = Path(tmp)
        runtime = base / "runtime"
        runtime.mkdir(mode=0o700)
        env = dict(os.environ, HOME=str(base / "home"), QT_QPA_PLATFORM="offscreen",
                   XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(base / "config"),
                   XDG_DATA_HOME=str(base / "data"))
        source = base / 'source é😀 "\n.bin'
        destination = base / "destination"
        destination.mkdir()
        with source.open("wb") as output:
            output.write(b"debut de fichier\n")
            output.seek(256 * 1024 * 1024 - 16)
            output.write(b"fin de fichier!\n")
        process = None
        with (base / "gui.log").open("w+") as log:
            def launch():
                """L’interface et le moteur utilisent le même journal privé après chaque lancement."""
                return subprocess.Popen([binary, "--background"], env=env, stdout=log, stderr=log)

            def request(value):
                """Une connexion par commande évite les réponses mélangées pendant les sondages."""
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(10)
                    client.connect(str(runtime / "gad.sock"))
                    client.sendall(json.dumps(value).encode() + b"\n")
                    response = b""
                    while b"\n" not in response:
                        chunk = client.recv(65536)
                        if not chunk:
                            raise ConnectionError("Socket fermée avant la réponse")
                        response += chunk
                    return json.loads(response)

            def wait(predicate):
                """L’échéance couvre aussi les machines lentes ; un enfant mort échoue immédiatement."""
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        log.flush()
                        raise AssertionError((base / "gui.log").read_text())
                    try:
                        state = request({"command": "status"})
                        if predicate(state):
                            return state
                    except (FileNotFoundError, ConnectionRefusedError):
                        pass
                    time.sleep(.002)
                raise AssertionError("Délai de reprise dépassé")

            try:
                process = launch()
                wait(lambda state: "jobs" in state)
                assert request({"command": "configure", "resume_interrupted": True, "keep_history": True}).get("ok")
                result = request({"command": "enqueue", "sources": [str(source)],
                                  "destination": str(destination), "mode": "copy", "verify_md5": True})
                assert "id" in result, result
                # Attendre une sauvegarde au milieu du fichier, puis arrêter sans nettoyage ni sauvegarde finale.
                def saved_prefix(_state):
                    """L’état en mémoire précède le fsync du journal : attendre un point complet du journal sur disque."""
                    saved_state = read_journal(base / "data/gad/state.json")
                    return any(
                        0 < ((entry.get("undo") or {}).get("resume") or {}).get("bytes", 0) < source.stat().st_size
                        for job in saved_state["jobs"] for entry in job["entries"])

                state = wait(saved_prefix)
                entry = state["jobs"][0]["entries"][0]
                partial = Path(entry["undo"]["staging"])
                assert partial.parent==destination and not partial.name.startswith("."),partial
                assert partial.name.endswith(".part"),partial
                inode = partial.stat().st_ino
                process.kill()
                process.wait(timeout=10)
                frozen = read_journal(base / "data/gad/state.json")
                saved = frozen["jobs"][0]["entries"][0]["undo"]["resume"]["bytes"]
                assert 0 < saved < source.stat().st_size
                process = launch()
                state = wait(lambda state: bool(state["jobs"]) and state["jobs"][0]["state"] in ("done", "error"))
                job = state["jobs"][0]
                assert job["state"] == "done", job
                entry = job["entries"][0]
                output = destination / source.name
                assert entry["resumed_from"] == saved
                assert output.stat().st_ino == inode, "Le temporaire a été recopié plutôt que repris"
                with source.open("rb") as original, output.open("rb") as copied:
                    assert hashlib.file_digest(original, "md5").digest() == hashlib.file_digest(copied, "md5").digest()
                assert not partial.exists()
                print("OK : SIGKILL, relancement, reprise du même temporaire et comparaison MD5 des contenus.")
            finally:
                if process is not None and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)


if __name__ == "__main__":
    main()
