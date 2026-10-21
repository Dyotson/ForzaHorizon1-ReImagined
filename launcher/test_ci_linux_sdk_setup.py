"""Exercise CI dependency retries without installing packages or downloading tools."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).parent / "packaging/ci_linux_sdk_setup.sh"


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "requires a POSIX shell")
class LinuxSdkSetupTests(unittest.TestCase):
    def run_setup(self, failures, *args):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root / "commands"
            stub = r'''#!/usr/bin/env bash
name="${0##*/}"
printf '%s %s\n' "$name" "$*" >> "$CI_TEST_LOG"
if [ "$name" = id ]; then echo 0; exit 0; fi
if [ "$name" = apt-get ] && [[ "$*" == *g++-14* ]]; then
    n=0; [ ! -f "$CI_TEST_COUNT" ] || read -r n < "$CI_TEST_COUNT"
    n=$((n + 1)); echo "$n" > "$CI_TEST_COUNT"
    if [ "$n" -le "$CI_TEST_FAILURES" ]; then exit 100; fi
fi
'''
            for name in ("id", "apt-get", "add-apt-repository", "sleep", "python3"):
                target = root / name
                target.write_text(stub)
                target.chmod(0o755)
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"],
                       CI_TEST_LOG=str(log), CI_TEST_COUNT=str(root / "count"),
                       CI_TEST_FAILURES=str(failures))
            result = subprocess.run(["bash", str(SCRIPT), *args], env=env, text=True,
                                    capture_output=True, timeout=10)
            return result, log.read_text().splitlines()

    def test_transient_install_failure_recovers(self):
        result, commands = self.run_setup(2)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sum("g++-14" in row for row in commands), 3)
        self.assertEqual([row for row in commands if row.startswith("sleep ")],
                         ["sleep 10", "sleep 20"])
        self.assertEqual(sum(row.startswith("python3 ") for row in commands), 3)
        self.assertTrue(all("Acquire::Retries=3" in row for row in commands
                            if row.startswith("apt-get ")))

    def test_persistent_failure_stops_before_downloads(self):
        result, commands = self.run_setup(99)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sum("g++-14" in row for row in commands), 3)
        self.assertFalse(any(row.startswith("python3 ") for row in commands))

    def test_success_does_not_retry(self):
        result, commands = self.run_setup(0)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sum("g++-14" in row for row in commands), 1)
        self.assertFalse(any(row.startswith("sleep ") for row in commands))

    def test_cache_hit_installs_compiler_without_sdk_downloads(self):
        result, commands = self.run_setup(0, "--compiler-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        installs = [row for row in commands if row.startswith("apt-get ") and "install" in row]
        self.assertTrue(any(row.endswith("install -y g++-14") for row in installs))
        self.assertFalse(any("libvulkan-dev" in row for row in installs))
        self.assertFalse(any(row.startswith("python3 ") for row in commands))


if __name__ == "__main__":
    unittest.main()
