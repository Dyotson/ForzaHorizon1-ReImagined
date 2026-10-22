"""Editable frame rates commit with Qt's real validator and completion behavior."""
import os
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
try:
    from PySide6.QtCore import Qt
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication
except ImportError:
    QApplication = None


@unittest.skipIf(QApplication is None, "PySide6 is not installed")
class FrameRateControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def test_presets_and_typed_rates_commit_on_enter(self):
        from forza_launcher import schema
        from forza_launcher.ui.settings_page import make_control
        setting = next(s for s in schema.S if s.key == "frame_rate")
        values = []
        control = make_control(setting, 240, values.append)
        control.show()
        self.app.processEvents()
        try:
            presets = [30, 60, 75, 90, 100, 120, 144, 165, 180, 200, 240]
            self.assertEqual([control.itemData(i) for i in range(control.count())], [0] + presets)  # 0 = Match display
            # A matching preset autocompletes with " fps". The validator must
            # accept that text, or Qt never emits editingFinished on Enter.
            for typed, expected in [(str(v), v) for v in presets] + [
                    ("61", 61), ("119", 119), ("175", 175), ("237", 237),
                    ("45", 30), ("999", 240)]:
                with self.subTest(typed=typed):
                    values.clear()
                    control.lineEdit().selectAll()
                    QTest.keyClicks(control.lineEdit(), typed)
                    QTest.keyClick(control.lineEdit(), Qt.Key_Return)
                    self.app.processEvents()
                    self.assertTrue(values, "Enter did not commit the rate")
                    self.assertEqual(values[-1], expected)
                    self.assertEqual(control.currentText(), f"{expected} fps")
        finally:
            control.close()
