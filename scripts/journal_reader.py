"""Lecture du journal durable pour les diagnostics et les tests après arrêt forcé."""
import hashlib
import json
from pathlib import Path


def _raw_jobs(payload, field):
    """Retrouve les objets JSON exacts ; les réencoder changerait les bases des deltas."""
    text = payload.decode("utf-8")
    decoder = json.JSONDecoder()
    def space(index):
        while index < len(text) and text[index] in " \t\r\n":
            index += 1
        return index
    cursor = space(1)
    while text[cursor] != "}":
        key, cursor = decoder.raw_decode(text, cursor)
        cursor = space(cursor) + 1  # deux-points déjà validé par json.loads
        start = space(cursor)
        if key == field:
            cursor = space(start + 1)
            result = []
            while text[cursor] != "]":
                start = cursor
                value, cursor = decoder.raw_decode(text, cursor)
                result.append((value, text[start:cursor].encode("utf-8")))
                cursor = space(cursor)
                if text[cursor] == ",":
                    cursor = space(cursor + 1)
            return result
        _, cursor = decoder.raw_decode(text, start)
        cursor = space(cursor)
        if text[cursor] == ",":
            cursor = space(cursor + 1)
    return []


def _patch(patch, raw):
    """Les positions portent sur les octets UTF-8, comme dans le lecteur Rust."""
    if hashlib.md5(raw).hexdigest() != patch["base"]:
        raise ValueError("Base du delta incorrecte")
    offset, remove = patch["offset"], patch["remove"]
    if type(offset) is not int or type(remove) is not int or min(offset, remove) < 0 or offset + remove > len(raw):
        raise ValueError("Bornes du delta invalides")
    prefix, suffix = raw[:offset], raw[offset + remove:]
    prefix.decode("utf-8")
    suffix.decode("utf-8")
    result = prefix + patch["insert"].encode("utf-8") + suffix
    if hashlib.md5(result).hexdigest() != patch["result"]:
        raise ValueError("Résultat du delta incorrect")
    value = json.loads(result)
    if value["id"] != patch["id"]:
        raise ValueError("Identité du delta incorrecte")
    return value, result


def read_journal(path):
    """Superpose les deltas complets ; réessaie si une compaction remplace l’instantané."""
    path = Path(path)
    for _ in range(10):
        before = path.stat()
        state = json.loads(path.read_bytes())
        try:
            log = path.with_suffix(".json.wal").read_bytes()
        except FileNotFoundError:
            log = b""
        after = path.stat()
        if (before.st_ino, before.st_size, before.st_mtime_ns) == (after.st_ino, after.st_size, after.st_mtime_ns):
            break
    else:
        raise RuntimeError("Le journal est en cours de compaction")
    base = state.pop("_gad_journal_seq", 0)
    sequence = base
    jobs = {job["id"]: job for job in state["jobs"]}
    ids = [job["id"] for job in state["jobs"]]
    raw_jobs = {}
    for line in log.splitlines(keepends=True):
        if not line.endswith(b"\n"):
            break
        checksum, separator, payload = line[:-1].partition(b" ")
        if not separator or hashlib.md5(payload).hexdigest().encode() != checksum:
            raise ValueError("Enregistrement du journal endommagé")
        record = json.loads(payload)
        if record["sequence"] <= base:
            continue
        if record["sequence"] != sequence + 1:
            raise ValueError("Séquence du journal discontinue")
        ids = record["ids"]
        id_set = set(ids)
        if len(ids) != len(id_set):
            raise ValueError("Identifiant répété")
        jobs = {key: value for key, value in jobs.items() if key in id_set}
        raw_jobs = {key: value for key, value in raw_jobs.items() if key in id_set}
        if record.get("format") is None and isinstance(record.get("jobs"), list) and not record.get("full_jobs") and not record.get("patches"):
            field = "jobs"
        elif type(record.get("format")) is int and record["format"] == 2 and record.get("jobs") is None:
            field = "full_jobs"
        else:
            raise ValueError("Format du journal inconnu ou incohérent")
        updated = set()
        for job, raw in _raw_jobs(payload, field):
            key = job["id"]
            if key not in id_set or key in updated:
                raise ValueError("Lot hors de l’ordre ou répété")
            updated.add(key)
            jobs[key], raw_jobs[key] = job, raw
        for patch in record.get("patches", []):
            key = patch["id"]
            if key not in id_set or key in updated or key not in raw_jobs:
                raise ValueError("Base du delta absente ou répétée")
            updated.add(key)
            jobs[key], raw_jobs[key] = _patch(patch, raw_jobs[key])
        state = record["header"]
        sequence = record["sequence"]
    state["jobs"] = [jobs[key] for key in ids]
    return state
