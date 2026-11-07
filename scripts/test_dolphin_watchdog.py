#!/usr/bin/env python3
"""Tester les QAction et le minuteur sans utiliser les fenêtres ou fichiers du bureau."""
from pathlib import Path
import tempfile
import subprocess
import os
ROOT=Path(__file__).resolve().parent.parent
build=ROOT/'target/dolphin-plugin/cmake'
subprocess.run(['cmake','-S',ROOT/'native/dolphin','-B',build,'-DGAD_WATCHDOG_TESTS=ON'],check=True)
subprocess.run(['cmake','--build',build,'--parallel','2'],check=True)
with tempfile.TemporaryDirectory(prefix='gad-watchdog-test-') as tmp:
    root=Path(tmp);runtime=root/'runtime';runtime.mkdir(mode=0o700)
    env=dict(os.environ,HOME=tmp,XDG_DATA_HOME=str(root/'data'),XDG_CONFIG_HOME=str(root/'config'),XDG_CACHE_HOME=str(root/'cache'),XDG_RUNTIME_DIR=str(runtime),QT_QPA_PLATFORM='offscreen')
    subprocess.run(['dbus-run-session','--',build/'watchdog-test'],env=env,check=True,timeout=30)
