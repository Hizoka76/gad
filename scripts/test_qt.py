#!/usr/bin/env python3
"""Lance toutes les suites Qt, ou une sélection, avec compilation native mutualisée."""
import argparse
from qt_tests import SUITES, run_suites

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", action="append", choices=SUITES, help="Suite à exécuter ; répétable, toutes par défaut.")
    args = parser.parse_args()
    run_suites(args.suite or SUITES)
