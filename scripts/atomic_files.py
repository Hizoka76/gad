"""Publier un fichier généré sans réutiliser de temporaire préexistant."""
import os
from pathlib import Path
import tempfile


def publish_bytes(target, data, mode=0o644):
    """Écrire dans le même dossier puis remplacer le nom final, sans suivre son lien éventuel."""
    target = Path(target)
    descriptor, name = tempfile.mkstemp(prefix='.gad-install-', suffix='.install', dir=target.parent)
    staged = Path(name)
    try:
        with os.fdopen(descriptor, 'wb') as output:
            output.write(data)
            output.flush()
            os.fchmod(output.fileno(), mode)
        staged.replace(target)
    finally:
        staged.unlink(missing_ok=True)
