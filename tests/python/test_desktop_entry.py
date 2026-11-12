"""Le chemin du programme reste littéral dans les entrées du bureau KDE."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from desktop_entry import executable


class DesktopEntryTests(unittest.TestCase):
    def test_field_codes_in_path_are_literal(self):
        self.assertEqual(executable(Path('/home/user %u/100% Gad/gad')),
                         '"/home/user %%u/100%% Gad/gad"')

    def test_argument_and_value_escaping_are_distinct(self):
        self.assertEqual(executable('/a/$`"\\/gad'), '"/a/\\\\$\\\\`\\\\"\\\\\\\\/gad"')

    def test_control_characters_do_not_create_desktop_lines(self):
        value = executable('/a\nComment=other\r\t/gad')
        self.assertEqual(value, '"/a\\nComment=other\\r\\t/gad"')
        self.assertNotIn('\n', value)

    def test_nul_is_rejected(self):
        with self.assertRaises(ValueError):
            executable('/invalid\0/gad')
