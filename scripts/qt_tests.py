from resources import generate
"""Met en cache les objets Qt et exécute les tests sur un bus privé avec des répertoires XDG isolés."""
import os
import fcntl
import hashlib
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SUITES = ("clipboard", "notifications", "row_actions", "konqueror")


def run_suites(names):
    """Réutilise les objets natifs entre tests de widgets ; aucune notification ni ouverture réelle n’est produite."""
    with tempfile.TemporaryDirectory(prefix="gad-qt-tests-") as temp:
        output = Path(temp)
        packages = ["Qt6Widgets", "Qt6Network", "Qt6Test", "Qt6DBus", "KF6WindowSystem"]
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", *packages], text=True))
        qt_tools = Path(subprocess.check_output(["qmake6", "-query", "QT_INSTALL_LIBEXECS"], text=True).strip())
        compiler = ["c++", "-std=c++17", "-O1", "-fPIC", "-Wall", "-Wextra", '-DGAD_VERSION="test-version"', "-I", str(output)]
        # Un cache adressé par contenu accélère les régressions sans partager données ni bus de test.
        include_widgets=any(name!="notifications" for name in names)
        digest=hashlib.sha256(repr((flags,include_widgets,str(qt_tools))).encode())
        for path in [Path(__file__),ROOT / "scripts/resources.py",*sorted((ROOT / "native").rglob("*")),ROOT / "assets/icons/gad.qrc",ROOT / "assets/icons/gad.png",*sorted((ROOT / "assets/flags").glob("*.png")),*sorted((ROOT / "translations").glob("*"))]:
            if path.is_file():digest.update(path.read_bytes())
        digest.update(subprocess.check_output(["pkg-config","--modversion",*packages]))
        digest.update(subprocess.check_output(["c++","--version"]))
        cache=ROOT / "target/qt-test-cache" / digest.hexdigest()
        cache.mkdir(parents=True,exist_ok=True)
        objects=[]
        with (cache / "build.lock").open("w") as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            moc=cache / "moc_notifications.cpp"
            common_sources=[moc,cache / "gad_assets.cpp"]
            if include_widgets:common_sources += [*sorted((ROOT / "native").glob("*.cpp"))]
            if not (cache / "complete").exists():
                subprocess.run([qt_tools / "moc",ROOT / "native/notifications.h","-o",moc],check=True)
                generate(cache / "gad.qrc")
                subprocess.run([qt_tools / "rcc","--name","gad_assets",cache / "gad.qrc","-o",cache / "gad_assets.cpp"],check=True)
                for source in common_sources:
                    subprocess.run([*compiler,*flags,"-c",source,"-o",cache / (source.stem+".o")],check=True)
                (cache / "complete").touch()
            objects=[cache / (source.stem+".o") for source in common_sources]
        for name in names:
            print(f"Test Qt : {name}", flush=True)
            source = ROOT / "tests" / f"{name}.cpp"
            moc_name = "clipboard_test.moc" if name == "clipboard" else f"{name}_test.moc"
            subprocess.run([qt_tools / "moc", source, "-o", output / moc_name], check=True)
            binary = output / (name + "-test")
            # Le double notifications ne fournit pas le moteur Rust : ne pas lui lier la fenêtre.
            linked = [] if name == "konqueror" else (objects if name != "notifications" else [cache / "moc_notifications.o",cache / "gad_assets.o"])
            subprocess.run([*compiler, source, *linked, "-o", binary, *flags], check=True)
            private = output / name
            runtime = private / "runtime"
            runtime.mkdir(parents=True, mode=0o700)
            env = dict(os.environ, QT_QPA_PLATFORM="offscreen", GAD_LANGUAGE="fr_FR", XDG_CONFIG_HOME=str(private / "config"),
                       XDG_DATA_HOME=str(private / "data"), XDG_CACHE_HOME=str(private / "cache"), XDG_RUNTIME_DIR=str(runtime))
            subprocess.run(["dbus-run-session", "--", binary], env=env, check=True, timeout=30)
