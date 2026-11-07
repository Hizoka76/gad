#!/bin/sh
# Aperçu par défaut ; ./clean.sh --apply supprime les fichiers temporaires du projet.
set -eu
cd "$(dirname "$0")"
exec python3 scripts/clean.py "$@"
