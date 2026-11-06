#!/usr/bin/env python3
"""Compatibilité : lance la suite Qt du presse-papiers et de la fenêtre, isolée du bureau."""
from qt_tests import run_suites

if __name__ == "__main__":
    run_suites(["clipboard"])
