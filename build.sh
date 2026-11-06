#!/bin/sh
# Compiler le programme sans modifier l’installation de l’utilisateur.
set -eu
cd "$(dirname "$0")"
python3 scripts/translations.py --check
exec cargo build --release --locked "$@"
