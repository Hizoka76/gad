#!/usr/bin/env python3
"""Point d’entrée des tests Gad : groupes indépendants, journaux et rapports JSON/JUnit, sans dépendance Python externe."""
import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent


@dataclass(frozen=True)
class Step:
    """Commande structurée et dépendances ; aucun argument n’est évalué par un shell."""
    name: str
    args: tuple
    dependencies: tuple = ()


def plan(group, rust_filter=None):
    """Construit les étapes explicites ; les anciens scripts restent utilisables séparément."""
    python = sys.executable
    rust = Step("rust-unit", ("cargo", "test", "--locked", *([rust_filter] if rust_filter else [])))
    python_unit = Step("python-unit", (python, "-m", "unittest", "discover", "-s", "tests/python", "-v"))
    qt = Step("qt-unit", (python, "scripts/test_qt.py"))
    watchdog = Step("dolphin-watchdog", (python, "scripts/test_dolphin_watchdog.py"))
    build = Step("integration-build", ("cargo", "build", "--locked"))
    integration = Step("integration", (python, "scripts/test_integration.py"), (build.name,))
    cli_settings = Step("cli-settings", (python, "scripts/test_cli_settings.py"), (build.name,))
    lint = [Step("rust-format", ("cargo", "fmt", "--check")), Step("rust-clippy", ("cargo", "clippy", "--locked", "--all-targets", "--", "-D", "warnings"))]
    groups = {"unit": [rust, python_unit], "python": [python_unit], "qt": [qt, watchdog], "integration": [build, integration, cli_settings],
              "lint": lint, "all": [*lint, rust, python_unit, qt, watchdog, build, integration, cli_settings],
              "bench": [Step("history-benchmark", ("cargo", "test", "--locked", "benchmark_large_history_polling", "--", "--ignored", "--nocapture"))]}
    return groups[group]


def execute_steps(steps, report_dir, *, keep_going=False, cwd=ROOT, env=None):
    """Exécute sans shell, conserve chaque sortie et saute les étapes dont une dépendance a échoué."""
    # Refuser un plan incohérent avant exécution, pour ne jamais produire un faux succès constitué de skips.
    known = set()
    for step in steps:
        if not step.name or step.name in (".", "..") or Path(step.name).name != step.name or step.name in known:
            raise ValueError(f"Nom d’étape invalide ou dupliqué : {step.name}")
        if any(name not in known for name in step.dependencies):
            raise ValueError(f"Dépendance absente ou placée après son utilisation : {step.name}")
        known.add(step.name)
    report_dir = Path(report_dir)
    report_dir.mkdir(parents=True, exist_ok=True)
    results, statuses = [], {}
    stopped = False
    for step in steps:
        blocked = stopped or any(statuses.get(name) != "passed" for name in step.dependencies)
        result = {"name": step.name, "command": list(map(str, step.args)), "status": "skipped", "seconds": 0.0, "returncode": None}
        if not blocked:
            print(f"\n[{step.name}] {shlex.join(map(str, step.args))}", flush=True)
            start = time.monotonic()
            log = report_dir / (step.name + ".log")
            try:
                with log.open("w", encoding="utf-8") as output:
                    # Les sorties restent visibles pendant l’exécution et disponibles après un échec.
                    with subprocess.Popen(step.args, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                          text=True, encoding="utf-8", errors="replace") as process:
                        for line in process.stdout:
                            print(line, end="", flush=True)
                            output.write(line)
                        code = process.wait()
            except OSError as error:
                code = 127
                log.write_text(str(error) + "\n", encoding="utf-8")
                print(error, file=sys.stderr)
            result.update(status="passed" if code == 0 else "failed", seconds=time.monotonic() - start, returncode=code, log=str(log))
            if code != 0 and not keep_going:
                stopped = True
        results.append(result)
        statuses[step.name] = result["status"]
    return results


def write_reports(results, report_dir):
    """Le JUnit contient une entrée par étape ; les détails de chaque test restent dans les journaux."""
    report_dir = Path(report_dir)
    report_dir.mkdir(parents=True, exist_ok=True)
    data = {"generated_at": datetime.now(timezone.utc).isoformat(), "results": results}
    (report_dir / "results.json").write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    suite = ET.Element("testsuite", name="Gad", tests=str(len(results)), failures=str(sum(r["status"] == "failed" for r in results)),
                       skipped=str(sum(r["status"] == "skipped" for r in results)), time=str(sum(r["seconds"] for r in results)))
    for result in results:
        case = ET.SubElement(suite, "testcase", classname="gad.validation", name=result["name"], time=str(result["seconds"]))
        if result["status"] == "skipped":
            ET.SubElement(case, "skipped", message="Étape précédente ou dépendance en échec")
        elif result["status"] == "failed":
            ET.SubElement(case, "failure", message=f"Code de sortie {result['returncode']}")
        if "log" in result:
            output = Path(result["log"]).read_text(encoding="utf-8", errors="replace")
            # Écarter les contrôles interdits par XML 1.0 ; les journaux bruts restent complets.
            output = "".join(c for c in output if c in "\n\t\r" or 0x20 <= ord(c) <= 0xD7FF
                             or 0xE000 <= ord(c) <= 0xFFFD or 0x10000 <= ord(c) <= 0x10FFFF)
            ET.SubElement(case, "system-out").text = output
    ET.ElementTree(suite).write(report_dir / "junit.xml", encoding="utf-8", xml_declaration=True)


def doctor():
    """Signale les prérequis sans installation ni changement des préférences du bureau."""
    missing = []
    for tool in ("cargo", "c++", "cmake", "pkg-config", "qmake6", "dbus-run-session"):
        available = shutil.which(tool) is not None
        print(f"{'OK' if available else 'MANQUANT'} : {tool}")
        if not available:
            missing.append(tool)
    if shutil.which("pkg-config"):
        for package in ("Qt6Widgets", "Qt6Network", "Qt6DBus", "Qt6Test", "KF6WindowSystem"):
            available = subprocess.run(["pkg-config", "--exists", package]).returncode == 0
            print(f"{'OK' if available else 'MANQUANT'} : {package}")
            if not available:
                missing.append(package)
    if shutil.which("qmake6"):
        query = subprocess.run(["qmake6", "-query", "QT_INSTALL_LIBEXECS"], capture_output=True, text=True)
        for name in ("moc", "rcc"):
            available = query.returncode == 0 and os.access(Path(query.stdout.strip()) / name, os.X_OK)
            print(f"{'OK' if available else 'MANQUANT'} : Qt {name}")
            if not available:
                missing.append(name)
    if shutil.which("cargo"):
        for command in ("fmt", "clippy"):
            available = subprocess.run(["cargo", command, "--version"], capture_output=True).returncode == 0
            print(f"{'OK' if available else 'MANQUANT'} : cargo {command}")
            if not available:
                missing.append(command)
    return 1 if missing else 0


def main(argv=None):
    """Retourne un code exploitable par make, un IDE ou une intégration continue."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("group", nargs="?", default="all", choices=("all", "unit", "python", "qt", "integration", "lint", "bench", "doctor"))
    parser.add_argument("--filter", help="Filtre de nom des tests Rust (groupes unit ou all).")
    parser.add_argument("--keep-going", action="store_true", help="Continuer les étapes indépendantes après un échec.")
    parser.add_argument("--report-dir", type=Path, default=ROOT / "target/test-results", help="Répertoire des journaux et rapports JSON/JUnit.")
    parser.add_argument("--list", action="store_true", help="Afficher les commandes sans les exécuter.")
    args = parser.parse_args(argv)
    if args.group == "doctor":
        return doctor()
    if args.filter and args.group not in ("unit", "all"):
        parser.error("--filter concerne les tests Rust des groupes unit ou all")
    steps = plan(args.group, args.filter)
    if args.list:
        for step in steps:
            print(f"{step.name}: {shlex.join(map(str, step.args))}")
        return 0
    env = dict(os.environ, CARGO_TERM_COLOR="never")
    results = execute_steps(steps, args.report_dir, keep_going=args.keep_going, env=env)
    write_reports(results, args.report_dir)
    print(f"\nRapports : {args.report_dir.resolve()}")
    return 1 if any(result["status"] == "failed" for result in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
