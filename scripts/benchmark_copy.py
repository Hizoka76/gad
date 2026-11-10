#!/usr/bin/env python3
"""Mesure des copies privées de petits fichiers, avec comparaison facultative de deux binaires.

Exemple : dbus-run-session -- python3 scripts/benchmark_copy.py --binary target/release/gad
Les contenus et la conservation des sources sont vérifiés après chaque transfert.
"""
import argparse
import json
from pathlib import Path
import tempfile
import time
import benchmark_rename as benchmark


def run(base, binary, files, timeout, folders=1, reorder=False, verify_md5=False):
    """L’instance, les préférences, les fichiers et le journal sont propres à cet essai."""
    benchmark.BINARY = binary
    source = benchmark.fixture(base, "source", files, folders)
    destination = base / "destination"
    destination.mkdir()
    instance = benchmark.Instance(base)
    try:
        start = time.monotonic()
        result = instance.request({"command": "enqueue", "mode": "copy", "sources": [str(source)],
                                   "destination": str(destination), "background": not reorder,
                                   "prepare_only": reorder, "verify_md5": verify_md5})
        if reorder:
            # Préparer sans écriture, puis persister un ordre personnalisé avant démarrage.
            while True:
                state = instance.request({"command": "status"})
                job = next(job for job in state["jobs"] if job["id"] == result["id"])
                if job["state"] in ("error", "cancelled"):
                    raise RuntimeError(job["error"])
                if job["state"] == "waiting" and job["entries"]:
                    break
                if time.monotonic() - start > timeout:
                    raise RuntimeError("Délai de préparation dépassé")
                time.sleep(.01)
            index = next(i for i, entry in enumerate(job["entries"]) if entry["kind"] == "file")
            instance.request({"command": "reorder_rows", "action": "last",
                              "rows": [{"job": result["id"], "index": index}]})
            assert instance.request({"command": "status"})["row_order"]
            instance.request({"command": "pause", "paused": False})
        while True:
            state = instance.request({"command": "status"})
            job = next(job for job in state["jobs"] if job["id"] == result["id"])
            if job["state"] in ("error", "cancelled"):
                raise RuntimeError(job["error"])
            if job["state"] == "done" and state["queue_guard"]["idle"]:
                break
            if time.monotonic() - start > timeout:
                raise RuntimeError("Délai de copie dépassé")
            time.sleep(.01)
        elapsed = 1000 * (time.monotonic() - start)
        assert source.exists() and not job["fast_move"]
        for path in source.rglob("*"):
            if path.is_file():
                assert (destination / source.name / path.relative_to(source)).read_bytes() == path.read_bytes()
            elif path.is_dir():
                assert (destination / source.name / path.relative_to(source)).is_dir()
        if verify_md5:
            import hashlib
            expected = hashlib.md5(b"benchmark").hexdigest()
            assert all(entry["md5_source"] == entry["md5_destination"] == expected
                       for entry in job["entries"] if entry["kind"] == "file")
        return {"verify_md5": verify_md5, "files": files, "folders": folders, "reordered": reorder, "complete_ms": elapsed,
                "wal_segment_bytes": (base / "data/gad/state.json.wal").stat().st_size,
                "snapshot_bytes": (base / "data/gad/state.json").stat().st_size}
    finally:
        instance.close()


def main():
    """La comparaison alterne les versions ; aucune instance du bureau n’est contactée."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=benchmark.BINARY)
    parser.add_argument("--compare", type=Path, help="Ancien binaire à tester avant le nouveau à chaque essai")
    parser.add_argument("--files", type=int, default=100)
    parser.add_argument("--folders", type=int, default=1)
    parser.add_argument("--reorder", action="store_true", help="Personnaliser l’ordre des lignes avant de démarrer")
    parser.add_argument("--verify-md5", action="store_true", help="Activer et contrôler la vérification MD5")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--label", default="copy-")
    args = parser.parse_args()
    if min(args.files, args.folders, args.repeat, args.timeout) <= 0:
        parser.error("Tailles, durée et nombre d’essais doivent être positifs")
    if not args.label or Path(args.label).name != args.label or args.label in (".", ".."):
        parser.error("Le label doit être un préfixe de nom de fichier")
    versions = [("current", args.binary.resolve())]
    if args.compare:
        versions = [("before", args.compare.resolve()), ("after", args.binary.resolve())]
    out = benchmark.ROOT / "target/copy-performance"
    out.mkdir(parents=True, exist_ok=True)
    results = []
    for iteration in range(args.repeat):
        # Contrebalancer la première version pour limiter les effets d'ordre et de cache.
        for label, binary in (versions if iteration % 2 == 0 else list(reversed(versions))):
            with tempfile.TemporaryDirectory(prefix="copy-", dir=out) as temporary:
                row = run(Path(temporary), binary, args.files, args.timeout, args.folders, args.reorder, args.verify_md5)
                row.update(version=label, iteration=iteration)
                results.append(row)
                print(json.dumps(row), flush=True)
    (out / (args.label + "results.json")).write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
