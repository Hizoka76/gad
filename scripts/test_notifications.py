#!/usr/bin/env python3
"""Compatibilité : lance la suite de notifications KDE sur un bus privé."""
from qt_tests import run_suites

if __name__ == "__main__":
    run_suites(["notifications"])
