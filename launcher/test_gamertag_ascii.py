import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parent))

from forza_launcher import gamertag, schema
from forza_launcher.config import Config


class AsciiGamertagTests(unittest.TestCase):
    def test_clean_names(self):
        cases = {
            "Dyotson": "Dyotson", "  Two   Words ": "Two Words", "José Müller": "Jose Muller",
            "Zalgo" + "̀́" * 20: "Zalgo", "中文": "", "\U0001F600": "",
            "a‮b​c": "abc", "x" * 40: "x" * 15, "Abcdefghijklmn Pqr": "Abcdefghijklmn",
            "tab\there\n": "tab here", None: "",
        }
        for raw, want in cases.items():
            self.assertEqual(gamertag.clean_name(raw), want, repr(raw))

    def test_check_name_messages(self):
        self.assertEqual(gamertag.check_name("Driver"), ("Driver", ""))
        cleaned, msg = gamertag.check_name("José")
        self.assertEqual(cleaned, "Jose")
        self.assertIn("letters, digits and spaces", msg)
        self.assertIn("login name", gamertag.check_name("中文")[1])

    def test_coerce_only_touches_gamertag(self):
        self.assertEqual(schema.coerce(schema.BY_KEY["gamertag"], "Jürgen"), "Jurgen")
        other = next(s for s in schema.S if s.kind == "str" and s.key != "gamertag")
        self.assertEqual(schema.coerce(other, "中" * 5), "中" * 5)

    def test_saved_invalid_gamertag_is_cleaned_at_load(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "launcher.json"
            path.write_text(json.dumps({"settings": {"gamertag": "Zed" + "́" * 30}}))
            cfg = Config(path)
            self.assertEqual(cfg.settings["gamertag"], "Zed")
            self.assertEqual(cfg.gamertag_cleaned[1], "Zed")
            path.write_text(json.dumps({"settings": {"gamertag": "Fine"}}))
            self.assertIsNone(Config(path).gamertag_cleaned)

    def test_widget_cleans_and_reports(self):
        from PySide6.QtWidgets import QApplication
        from forza_launcher.ui.settings_page import make_control
        app = QApplication.instance() or QApplication([])
        values = []
        w = make_control(schema.BY_KEY["gamertag"], "", values.append)
        w.setText("José 中文")
        w.editingFinished.emit()
        self.assertEqual(w.text(), "Jose")
        self.assertEqual(values[-1], "Jose")
        w.deleteLater()
        app.processEvents()

    def test_widget_limits_while_typing(self):
        from PySide6.QtWidgets import QApplication
        from forza_launcher.ui.settings_page import make_control
        app = QApplication.instance() or QApplication([])
        w = make_control(schema.BY_KEY["gamertag"], "", lambda v: None)
        self.assertEqual(w.maxLength(), 15)
        for typed, kept in (("Jo\u2603s\u00e9 ", "Jose "), ("a\u0301\u0301\u0301b", "ab"),
                            ("\u5343\u7fbd", ""), ("Mr X 99", "Mr X 99")):
            with self.subTest(typed=typed):
                w.setText(typed)
                w.textEdited.emit(typed)
                self.assertEqual(w.text(), kept)
        self.assertEqual(gamertag.filter_typing("x" * 20), "x" * 15)
        w.deleteLater()
        app.processEvents()


if __name__ == "__main__":
    unittest.main()
