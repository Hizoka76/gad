"""Tests unitaires des emplacements utilisateur, avec environnement et dossiers temporaires."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import xdg_paths


class XdgPathsTest(unittest.TestCase):
    def test_defaults_and_invalid_values_never_use_current_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            for value in ("", "relative/path"):
                with self.subTest(value=value), patch.dict(os.environ, {"HOME": temp, "XDG_DATA_HOME": value, "XDG_CONFIG_HOME": value}, clear=True):
                    self.assertEqual(xdg_paths.data_home(), Path(temp) / ".local/share")
                    self.assertEqual(xdg_paths.config_home(), Path(temp) / ".config")

    def test_absolute_overrides_are_independent(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            with patch.dict(os.environ, {"HOME": str(base / "home"), "XDG_DATA_HOME": str(base / "data"), "XDG_CONFIG_HOME": str(base / "config")}, clear=True):
                self.assertEqual(xdg_paths.data_home(), base / "data")
                self.assertEqual(xdg_paths.config_home(), base / "config")

    def test_unset_overrides_use_user_defaults(self):
        with tempfile.TemporaryDirectory() as temp, patch.dict(os.environ, {"HOME": temp}, clear=True):
            self.assertEqual(xdg_paths.data_home(), Path(temp) / ".local/share")

    def test_invalid_home_is_refused_instead_of_writing_relative_files(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(Path, "home", return_value=Path("relative-home")):
            with self.assertRaises(RuntimeError):
                xdg_paths.data_home()
