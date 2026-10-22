"""Configure-only opt-in matrix; synthetic includes, no SDK or game inputs.

CMake checks the host C++ compiler, but no target is built or executed.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "forza-recomp/cmake/Fh2Pause.cmake"


@unittest.skipUnless(shutil.which("cmake"), "CMake is required")
class PauseConfiguration(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="fh2 pause config ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.source.mkdir()
        self.navigation = self.root / "private navigation"
        self.events = self.root / "private events"
        self.navigation.mkdir()
        self.events.mkdir()
        (self.navigation / "pause_navigation.inc").touch()
        (self.events / "grid_events.inc").touch()
        (self.source / "dummy.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(pause_configuration LANGUAGES CXX)\n"
            'set(FORZA_TITLE_UPDATE 0 CACHE STRING "Game version")\n'
            f'include("{HELPER.as_posix()}")\n'
            "forza_validate_fh2_pause()\n"
            "add_executable(dummy dummy.cpp)\n"
            "forza_apply_fh2_pause(dummy)\n"
            'foreach(property COMPILE_DEFINITIONS INCLUDE_DIRECTORIES)\n'
            '  get_target_property(value dummy ${property})\n'
            '  if(NOT value)\n'
            '    set(value "")\n'
            '  endif()\n'
            '  file(WRITE "${CMAKE_BINARY_DIR}/${property}.txt" "${value}")\n'
            "endforeach()\n",
            encoding="utf-8")

    def configure(self, *, version=None, navigation=False, events=False, source=None):
        command = ["cmake", "-S", str(source or self.source), "-B", str(self.build)]
        if version is not None:
            command.append(f"-DFORZA_TITLE_UPDATE={version}")
        if navigation:
            command.append(f"-DFORZA_FH2_NAVIGATION_DIR={self.navigation.as_posix()}")
        if events:
            command.append(f"-DFORZA_FH2_GRID_EVENTS_DIR={self.events.as_posix()}")
        return subprocess.run(command, capture_output=True, text=True, timeout=120)

    def assert_configuration(self, result, definitions=(), includes=()):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for property_name, expected in (("COMPILE_DEFINITIONS", definitions),
                                         ("INCLUDE_DIRECTORIES", includes)):
            value = (self.build / f"{property_name}.txt").read_text(encoding="utf-8")
            self.assertEqual(value.split(";") if value else [], list(expected))

    def assert_rejected(self, result, diagnostic):
        self.assertNotEqual(result.returncode, 0)
        output = " ".join((result.stdout + result.stderr).split())
        self.assertIn(diagnostic, output)

    def test_base_default_is_disabled(self):
        self.assert_configuration(self.configure())

    def test_tu4_default_is_disabled(self):
        self.assert_configuration(self.configure(version=4))

    def test_navigation_only_tu4(self):
        self.assert_configuration(self.configure(version=4, navigation=True),
                                  ["FORZA_FH2_PAUSE=1"], [self.navigation.as_posix()])

    def test_full_scene_tu4(self):
        self.assert_configuration(self.configure(version=4, navigation=True, events=True),
                                  ["FORZA_FH2_PAUSE=1", "FORZA_FH2_PAUSE_SCENE=1"],
                                  [self.navigation.as_posix(), self.events.as_posix()])

    def test_base_rejects_navigation(self):
        self.assert_rejected(self.configure(version=0, navigation=True),
                             "requires FH1 Title Update 4")

    def test_base_rejects_full_scene(self):
        self.assert_rejected(self.configure(version=0, navigation=True, events=True),
                             "requires FH1 Title Update 4")

    def test_grid_requires_navigation(self):
        self.assert_rejected(self.configure(version=4, events=True),
                             "The FH2 scene requires FORZA_FH2_NAVIGATION_DIR")

    def test_missing_navigation_translation(self):
        (self.navigation / "pause_navigation.inc").unlink()
        self.assert_rejected(self.configure(version=4, navigation=True),
                             "Generate pause_navigation.inc")

    def test_missing_grid_translation(self):
        (self.events / "grid_events.inc").unlink()
        self.assert_rejected(self.configure(version=4, navigation=True, events=True),
                             "Generate grid_events.inc")

    def test_real_entry_point_rejects_before_sdk_or_codegen(self):
        # The real project must diagnose the option error even in a checkout
        # with no SDK or generated game sources, not merely the dummy project.
        result = self.configure(version=4, events=True, source=ROOT / "forza-recomp")
        self.assert_rejected(result, "The FH2 scene requires FORZA_FH2_NAVIGATION_DIR")
        self.assertNotIn("First configure: running codegen", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
