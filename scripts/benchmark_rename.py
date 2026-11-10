#!/usr/bin/env python3
"""Compare renommage POSIX, processus KIO et Gad sur des arbres privés du même système de fichiers.

Usage : dbus-run-session -- python3 scripts/benchmark_rename.py --binary target/release/gad --label final-
Ajouter --visible pour mesurer aussi la construction de la liste Qt.
"""
from pathlib import Path
import argparse
import json
import os
import socket
import statistics
import subprocess
import tempfile
import time
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'target/rename-performance'
BINARY = Path(os.environ.get('GAD_TEST_BINARY', str(ROOT / 'target/release/gad')))
LABEL = os.environ.get('GAD_BENCH_LABEL', 'latest-')
VISIBLE = False

def fixture(parent, name, files, folders):
    """Arbre privé, entièrement supprimé après chaque mesure."""
    root = parent / name
    root.mkdir()
    for d in range(folders):
        directory = root / f'd{d:05}'
        directory.mkdir()
        count = files // folders + int(d < files % folders)
        for f in range(count):
            (directory / f'f{f:05}').write_bytes(b'benchmark')
    return root

class Instance:

    def __init__(self, base, trace=False):
        self.base = base
        self.runtime = base / 'runtime'
        self.runtime.mkdir(mode=0o700)
        self.env = dict(os.environ, QT_QPA_PLATFORM='offscreen', HOME=str(base), XDG_RUNTIME_DIR=str(self.runtime), XDG_CONFIG_HOME=str(base / 'config'), XDG_DATA_HOME=str(base / 'data'), XDG_CACHE_HOME=str(base / 'cache'))
        config = base / 'config/Gad/Gad.conf'
        config.parent.mkdir(parents=True)
        config.write_text('[General]\nverifyMd5=false\nverifyProperties=false\nverifySpace=false\nverifyFastMove=false\nkeepHistory=true\nprogressMessage=false\n')
        self.log = (base / 'app.log').open('w')
        args = [str(BINARY), *([] if VISIBLE else ['--background'])]
        if trace:
            args = ['strace', '-f', '-ttt', '-e', 'trace=openat,newfstatat,statx,getdents64,renameat2,fsync,fdatasync', '-o', str(OUT / (LABEL + 'syscalls.log')), *args]
        self.p = subprocess.Popen(args, env=self.env, stdout=self.log, stderr=self.log, start_new_session=True)
        try:
            end = time.monotonic() + 20
            while not (self.runtime / 'gad.sock').exists():
                if self.p.poll() is not None or time.monotonic() > end:
                    raise RuntimeError('Démarrage impossible')
                time.sleep(0.01)
            self.request({'command': 'status'})
        except BaseException:
            self.close()
            raise

    def request(self, data):
        """Une connexion IPC par requête ; aucun accès à l’instance du bureau."""
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(60)
            client.connect(str(self.runtime / 'gad.sock'))
            client.sendall(json.dumps(data).encode() + b'\n')
            data = b''
            while b'\n' not in data:
                block = client.recv(1048576)
                if not block:
                    raise RuntimeError('Socket interrompue')
                data += block
            response = json.loads(data)
            if 'error' in response:
                raise RuntimeError(response)
            return response

    def move(self, source, destination):
        """Chronomètre l’acceptation, le renommage réel puis la finalisation visible par IPC."""
        inode = source.stat().st_ino
        target = destination / source.name
        wall_start = time.time()
        start = time.monotonic()
        reply = self.request({'command': 'enqueue', 'mode': 'move', 'sources': [str(source)], 'destination': str(destination), 'background': True})
        accepted = time.monotonic()
        while source.exists() or not target.exists():
            if time.monotonic() - start > 120:
                raise RuntimeError('Délai renommage dépassé')
            time.sleep(0.001)
        renamed = time.monotonic()
        while True:
            state = self.request({'command': 'status'})
            job = next((j for j in state['jobs'] if j['id'] == reply['id']))
            if job['state'] in ('error', 'cancelled'):
                raise RuntimeError(job['error'])
            if job['state'] == 'done' and state['queue_guard']['idle']:
                break
            if time.monotonic() - start > 120:
                raise RuntimeError('Délai finalisation dépassé')
            time.sleep(0.01)
        end = time.monotonic()
        assert target.stat().st_ino == inode
        assert job['fast_move']
        assert job['entries'][0]['undo']['directory_move']
        return {'wall_start': wall_start, 'wall_end': time.time(), 'accept_ms': 1000 * (accepted - start), 'rename_ms': 1000 * (renamed - start), 'complete_ms': 1000 * (end - start), 'rows': len(job['entries']), 'journal_bytes': (self.base / 'data/gad/state.json').stat().st_size, 'wal_bytes': (self.base / 'data/gad/state.json.wal').stat().st_size if (self.base / 'data/gad/state.json.wal').exists() else 0}

    def close(self):
        """Termine uniquement notre instance, reconnue par son runtime privé."""
        try:
            self.request({'command': 'shutdown'})
        except Exception:
            pass
        import signal
        # Popen crée une session dédiée : même sous strace, ce groupe ne contient
        # que notre enfant et ses descendants, jamais le Gad de la session utilisateur.
        if self.p.poll() is None:
            try:
                os.killpg(self.p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(self.p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.p.wait(timeout=5)
        self.log.close()

def main():
    global BINARY, LABEL, VISIBLE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', action='store_true')
    parser.add_argument('--binary', type=Path, default=BINARY)
    parser.add_argument('--label', default=LABEL)
    parser.add_argument('--visible', action='store_true')
    args = parser.parse_args()
    BINARY = args.binary.resolve()
    LABEL = args.label
    VISIBLE = args.visible
    if Path(LABEL).name != LABEL or LABEL in ('.', '..'):
        parser.error('Label must be a filename prefix')
    OUT.mkdir(parents=True, exist_ok=True)
    results = []
    scenarios = [(1000, 1)] if args.trace else [(1, 1), (100, 1), (1000, 1), (5000, 1), (1000, 1000)]
    for files, dirs in scenarios:
        repeats = 1 if args.trace else 3
        for iteration in range(repeats):
            with tempfile.TemporaryDirectory(prefix='bench-', dir=OUT) as tmp:
                base = Path(tmp)
                source = fixture(base, 'source', files, dirs)
                destination = base / 'dest'
                destination.mkdir()
                target = destination / source.name
                native = []
                for _ in range(5):
                    start = time.perf_counter_ns()
                    source.rename(target)
                    native.append((time.perf_counter_ns() - start) / 1000000.0)
                    target.rename(source)
                kio_start = time.monotonic()
                subprocess.run(['kioclient', '--noninteractive', 'move', str(source), str(target)], env=dict(os.environ, QT_QPA_PLATFORM='offscreen', HOME=str(base), XDG_RUNTIME_DIR=str(base), XDG_CONFIG_HOME=str(base / 'kio-config'), XDG_DATA_HOME=str(base / 'kio-data'), XDG_CACHE_HOME=str(base / 'kio-cache')), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=30)
                kio_ms = (time.monotonic() - kio_start) * 1000
                target.rename(source)
                instance = Instance(base, trace=args.trace)
                try:
                    row = instance.move(source, destination)
                    row.update(files=files, folders=dirs, iteration=iteration, native_rename_ms=statistics.median(native), kio_process_ms=kio_ms)
                    if not args.trace:
                        later = fixture(base, 'small', 1, 1)
                        row['small_after_history'] = instance.move(later, destination)
                finally:
                    instance.close()
                results.append(row)
                print(json.dumps(row), flush=True)
        (OUT / (LABEL + ('trace-results.json' if args.trace else 'results.json'))).write_text(json.dumps(results, indent=2) + '\n')
if __name__ == '__main__':
    main()
