"""Contrôle les erreurs, dépendances et rapports du lanceur sans compiler ni lancer Gad."""
import json
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from run_tests import Step, execute_steps, plan, write_reports


class TestRunnerTest(unittest.TestCase):
    def setUp(self):
        # Les erreurs volontaires restent vérifiées sans polluer la sortie de la suite par des faux messages d’échec.
        for stream in ("stdout", "stderr"):
            capture = patch("sys." + stream, new=io.StringIO())
            capture.start()
            self.addCleanup(capture.stop)

    def test_failure_stops_following_steps_by_default(self):
        with tempfile.TemporaryDirectory() as temp:
            result = execute_steps([Step("failure", (sys.executable, "-c", "raise SystemExit(3)")),
                                    Step("later", (sys.executable, "-c", "print('not reached')"))], temp)
            self.assertEqual([r["status"] for r in result], ["failed", "skipped"])
            self.assertEqual(result[0]["returncode"], 3)

    def test_keep_going_still_skips_failed_dependencies(self):
        with tempfile.TemporaryDirectory() as temp:
            result = execute_steps([Step("build", (sys.executable, "-c", "raise SystemExit(4)")),
                                    Step("dependent", (sys.executable, "-c", "print('not reached')"), ("build",)),
                                    Step("independent", (sys.executable, "-c", "print('works')"))], temp, keep_going=True)
            self.assertEqual([r["status"] for r in result], ["failed", "skipped", "passed"])

    def test_json_and_junit_include_failure_and_unicode_logs(self):
        with tempfile.TemporaryDirectory() as temp:
            result = execute_steps([Step("failure", (sys.executable, "-c", "print('été <test>'); raise SystemExit(2)"))], temp)
            write_reports(result, temp)
            report = json.loads((Path(temp) / "results.json").read_text())
            self.assertEqual(report["results"][0]["status"], "failed")
            xml = ET.parse(Path(temp) / "junit.xml").getroot()
            self.assertEqual(xml.attrib["failures"], "1")
            self.assertIn("été <test>", xml.find("testcase/system-out").text)

    def test_arguments_are_passed_as_data_without_shell_interpretation(self):
        with tempfile.TemporaryDirectory() as temp:
            value = "spaces ; $(not-a-command) # ' quotes"
            result = execute_steps([Step("literal", (sys.executable, "-c", "import sys;print(sys.argv[1])", value))], temp)
            self.assertEqual(result[0]["status"], "passed")
            self.assertEqual((Path(temp) / "literal.log").read_text().strip(), value)

    def test_missing_program_is_a_reported_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            result = execute_steps([Step("missing", (str(Path(temp) / "not-a-program"),))], temp)
            self.assertEqual(result[0]["returncode"], 127)
            self.assertEqual(result[0]["status"], "failed")

    def test_integration_requires_its_build_and_filter_is_only_for_rust(self):
        integration = plan("integration")
        self.assertEqual(integration[1].dependencies, (integration[0].name,))
        unit = plan("unit", "rollback_")
        self.assertIn("rollback_", unit[0].args)
        self.assertNotIn("rollback_", unit[1].args)

    def test_invalid_dependencies_and_duplicate_names_cannot_look_successful(self):
        for steps in ([Step("dependent", ("unused",), ("absent",))], [Step("same", ("unused",)), Step("same", ("unused",))]):
            with tempfile.TemporaryDirectory() as temp, self.assertRaises(ValueError):
                execute_steps(steps, temp)

    def test_junit_remains_readable_with_binary_control_characters_in_logs(self):
        with tempfile.TemporaryDirectory() as temp:
            code = "import sys;sys.stdout.buffer.write(bytes([65,0,66])+chr(0xfffe).encode('utf-8')+b'C')"
            result = execute_steps([Step("controls", (sys.executable, "-c", code))], temp)
            write_reports(result, temp)
            text = ET.parse(Path(temp) / "junit.xml").getroot().find("testcase/system-out").text
            self.assertEqual(text, "ABC")
