"""Lancer un scénario d’intégration avec ses propres services D-Bus et répertoires utilisateur."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(script):
    """L’environnement privé précède le bus, donc aussi les services qu’il peut activer."""
    with tempfile.TemporaryDirectory(prefix='gad-test-bus-') as temporary:
        root = Path(temporary)
        (root / 'home').mkdir()
        (root / 'runtime').mkdir(mode=0o700)
        env = dict(os.environ, HOME=str(root / 'home'),
                   XDG_RUNTIME_DIR=str(root / 'runtime'), XDG_CONFIG_HOME=str(root / 'config'),
                   XDG_DATA_HOME=str(root / 'data'), XDG_CACHE_HOME=str(root / 'cache'),
                   XDG_CONFIG_DIRS=str(root / 'system-config'), XDG_DATA_DIRS='/usr/local/share:/usr/share',
                   QT_QPA_PLATFORM='offscreen', DBUS_SYSTEM_BUS_ADDRESS='unix:path=' + str(root / 'no-system-bus.sock'))
        subprocess.run(['dbus-run-session', '--', sys.executable, str(script), '--private-session'], env=env, check=True)
