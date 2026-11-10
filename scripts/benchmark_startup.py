#!/usr/bin/env python3
"""Mesure le démarrage et le pic mémoire avec un instantané ou un WAL synthétique.

Les lots sont terminés, sans opérations à reprendre ni sauvegardes à supprimer.
Toutes les instances et données sont privées. --records 0 mesure l'instantané seul.
"""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import time
import benchmark_rename as benchmark


def encoded(value):
    """Le même encodage conserve les bases JSON exactes des deltas."""
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def prepare(base, files, records, wal_format="legacy", custom_order=False):
    """Prépare des lots terminés ; le format delta commence toujours par une base complète."""
    entries = [{"source": str(base / "source" / f"file-{i}"),
                "destination": str(base / "destination" / f"file-{i}"),
                "kind": "file", "size": 9, "copied": 9, "verified": 0,
                "md5_source": None, "md5_destination": None, "state": "done", "error": ""}
               for i in range(files)]
    job = {"id": 1, "sources": [], "destination": str(base / "destination"),
           "mode": "copy", "verify_md5": False, "state": "done", "entries": entries,
           "error": "", "cancel": False, "policy": "ask"}
    header = {"keep_history": True, "next_id": 1, "paused": False}
    if custom_order:
        header["row_order"] = [{"job": 1, "index": index} for index in range(files)]
    path = base / "data/gad/state.json"
    path.parent.mkdir(parents=True, mode=0o700)
    path.write_text(json.dumps(dict(header, jobs=[job])), encoding="utf-8")
    if wal_format not in ("legacy", "delta"):
        raise ValueError("Unknown WAL format")
    marker = b"GAD-BENCH-000000"
    if wal_format == "delta":
        job["error"] = marker.decode()
        raw = encoded(job)
        offset = raw.rindex(b'"error":"' + marker + b'"') + len(b'"error":"')
    with path.with_suffix(".json.wal").open("wb") as output:
        for sequence in range(1, records + 1):
            if wal_format == "legacy":
                record = {"sequence": sequence, "header": header, "jobs": [job], "ids": [1]}
            else:
                record = {"sequence": sequence, "header": header, "jobs": None, "ids": [1],
                          "format": 2, "full_jobs": [job] if sequence == 1 else [], "patches": []}
                if sequence > 1:
                    replacement = f"GAD-BENCH-{sequence-1:06d}".encode()
                    result = raw[:offset] + replacement + raw[offset + len(marker):]
                    record["patches"].append({"id": 1, "base": hashlib.md5(raw).hexdigest(),
                        "result": hashlib.md5(result).hexdigest(), "offset": offset,
                        "remove": len(marker), "insert": replacement.decode()})
                    raw, marker = result, replacement
            payload = encoded(record)
            output.write(hashlib.md5(payload).hexdigest().encode() + b" " + payload + b"\n")
    return path.with_suffix(".json.wal").stat().st_size


def run(base, binary, files, records, wal_format, custom_order=False):
    """Une nouvelle instance par essai évite les pics cumulés du processus de mesure."""
    benchmark.BINARY = binary
    size = prepare(base, files, records, wal_format, custom_order)
    start = time.monotonic()
    instance = benchmark.Instance(base)
    try:
        elapsed = (time.monotonic() - start) * 1000
        state = instance.request({"command": "status"})
        assert len(state["jobs"]) == 1 and state["jobs"][0]["state"] == "done"
        assert len(state["jobs"][0]["entries"]) == files
        if wal_format == "delta" and records:
            assert state["jobs"][0]["error"] == f"GAD-BENCH-{records-1:06d}"
        memory = Path(f"/proc/{instance.p.pid}/status").read_text()
        peak = next(int(line.split()[1]) for line in memory.splitlines() if line.startswith("VmHWM:"))
        return {"startup_ms": elapsed, "peak_rss_kib": peak, "files": files,
                "records": records, "wal_format": wal_format, "custom_order": custom_order, "wal_bytes": size}
    finally:
        instance.close()


def main():
    """Contrebalance les versions ; aucune instance de la session utilisateur n'est contactée."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=benchmark.BINARY)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--files", type=int, default=5000)
    parser.add_argument("--records", type=int, default=14)
    parser.add_argument("--wal-format", choices=("legacy", "delta"), default="legacy")
    parser.add_argument("--custom-order", action="store_true", help="Inclure un ordre personnalisé de toutes les lignes")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--label", default="startup-")
    args = parser.parse_args()
    if min(args.files, args.repeat) < 1 or args.records < 0:
        parser.error("Tailles et répétitions positives, nombre d'enregistrements positif ou nul")
    if not args.label or Path(args.label).name != args.label or args.label in (".", ".."):
        parser.error("Le label doit être un préfixe de nom de fichier")
    versions = [("current", args.binary.resolve())]
    if args.compare:
        versions = [("before", args.compare.resolve()), ("after", args.binary.resolve())]
    out = benchmark.ROOT / "target/startup-performance"
    out.mkdir(parents=True, exist_ok=True)
    results = []
    for iteration in range(args.repeat):
        for label, binary in (versions if iteration % 2 == 0 else list(reversed(versions))):
            with tempfile.TemporaryDirectory(prefix="startup-", dir=out) as temporary:
                row = run(Path(temporary), binary, args.files, args.records, args.wal_format, args.custom_order)
                row.update(iteration=iteration, version=label)
                results.append(row)
                print(json.dumps(row), flush=True)
    (out / (args.label + "results.json")).write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
