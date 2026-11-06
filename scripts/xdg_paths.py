"""Résout les emplacements XDG comme le moteur : une valeur vide ou relative utilise le défaut utilisateur."""
import os
from pathlib import Path


def directory(variable, default):
    """N’accepte que les chemins absolus pour éviter des installations dépendant du dossier courant."""
    candidate = Path(os.environ.get(variable, ""))
    if candidate.is_absolute():
        return candidate
    home = Path.home()
    if not home.is_absolute():
        raise RuntimeError("Dossier utilisateur absolu introuvable")
    return home / default


def data_home():
    """Emplacement commun des lanceurs, icônes, raccourcis et du journal Gad."""
    return directory("XDG_DATA_HOME", ".local/share")


def config_home():
    """Emplacement des réglages et de l’entrée de démarrage automatique."""
    return directory("XDG_CONFIG_HOME", ".config")
