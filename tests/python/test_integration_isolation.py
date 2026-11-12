"""Les intégrations isolent aussi les services activés et restent inertes à l’import."""
import os
from pathlib import Path
import runpy
import subprocess
import sys
import unittest
from unittest.mock import patch, ANY

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
SCRIPTS = ('test_integration.py', 'test_resume_integration.py', 'test_cli_settings.py', 'test_remote_destinations.py')


class IntegrationIsolationTests(unittest.TestCase):
    def test_scripts_enter_private_bus_before_starting_the_exercise(self):
        for name in SCRIPTS:
            with self.subTest(script=name):
                script = ROOT / 'scripts' / name
                with patch.object(sys, 'argv', [str(script)]), patch('subprocess.run') as run, patch('importlib.util.find_spec', return_value=None):
                    runpy.run_path(str(script), run_name='__main__')
                run.assert_called_once_with(['dbus-run-session', '--', sys.executable, str(script), '--private-session'], env=ANY, check=True)
                env = run.call_args.kwargs['env']
                self.assertNotEqual(env['HOME'], os.environ.get('HOME'))
                self.assertEqual(env['QT_QPA_PLATFORM'], 'offscreen')
                private = Path(env['HOME']).parent
                for key in ('XDG_RUNTIME_DIR', 'XDG_DATA_HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME'):
                    self.assertEqual(Path(env[key]).parent, private)
                self.assertTrue(env['DBUS_SYSTEM_BUS_ADDRESS'].startswith('unix:path=' + str(private)))

    def test_failed_bus_does_not_start_the_exercise(self):
        for name in SCRIPTS:
            with self.subTest(script=name):
                script = ROOT / 'scripts' / name
                with patch.object(sys, 'argv', [str(script)]), patch('subprocess.run', side_effect=subprocess.CalledProcessError(1, 'dbus-run-session')):
                    with self.assertRaises(subprocess.CalledProcessError):
                        runpy.run_path(str(script), run_name='__main__')

    def test_import_does_not_launch_a_process(self):
        for name in (*SCRIPTS, 'test_dolphin_watchdog.py'):
            with self.subTest(script=name), patch('subprocess.run') as run, patch('subprocess.Popen') as spawn:
                runpy.run_path(str(ROOT / 'scripts' / name), run_name='gad_import_probe')
                run.assert_not_called()
                spawn.assert_not_called()
