"""Real filesystem/process tests for archive replacement, plus mocked Flatpak host transactions."""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import fetch, linux_update, update_helper, updater


@unittest.skipIf(os.name != "posix", "Linux installer needs POSIX filesystem semantics")
class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="forza update test ")
        self.root = Path(self.temp.name).resolve()
        self.dest = self.root / "installed app"
        self.dest.mkdir()
        self.config = self.root / "config"
        self.marker = self.root / "started"
        self.version = "1.2.3"
        self.write_launcher(self.dest, "old")
        self.restart = patch.object(update_helper, "restart", side_effect=lambda directory:
                                    subprocess.run([str(directory / "forza-launcher")], cwd=directory, check=True))
        self.restart.start()

    def tearDown(self):
        self.restart.stop()
        self.temp.cleanup()

    def write_launcher(self, dest, value, fail_at_install=False):
        script = ('#!/bin/sh\nif [ "${1:-}" = --version ]; then\n' +
                  ('  [ "$PWD" = "' + str(self.dest) + '" ] && exit 9\n' if fail_at_install else '') +
                  '  echo 1.2.3\nelse\n  echo ' + value + ' > "' + str(self.marker) + '"\nfi\n')
        file = dest / "forza-launcher"
        file.write_text(script)
        file.chmod(0o755)

    def bundle(self, fail_at_install=False, extra=None):
        stage = self.root / "package" / "ForzaHorizon1-ReImagined"
        stage.mkdir(parents=True)
        self.write_launcher(stage, "new", fail_at_install)
        (stage / "new-resource").write_text("new")
        archive = self.root / "update.tar.gz"
        with tarfile.open(archive, "w:gz") as bundle:
            bundle.add(stage, arcname=stage.name)
            if extra:
                extra(bundle)
        asset = updater.Asset(archive.name, "", hashlib.sha256(archive.read_bytes()).hexdigest(), archive.stat().st_size)
        return archive, asset

    def prepare(self, fail_at_install=False):
        archive, asset = self.bundle(fail_at_install)
        helper, job_file, job = linux_update.prepare_archive(archive, asset, self.version, self.dest, self.config)
        process = subprocess.Popen(["/bin/sh", "-c", "exit 0"])
        process.wait()
        job["parent_pid"] = process.pid
        job_file.write_text(json.dumps(job))
        return helper, job_file, job

    def wait_marker(self):
        deadline = time.monotonic() + 3
        while not self.marker.exists() and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertTrue(self.marker.exists())
        return self.marker.read_text().strip()

    def test_replace_backup_restart_and_preserve_user_data(self):
        data = self.root / "game data"
        data.mkdir()
        (data / "save").write_text("career")
        (self.dest / "obsolete-resource").write_text("old")
        _, _, job = self.prepare()
        update_helper.apply_archive(job)
        self.assertEqual(self.wait_marker(), "new")
        self.assertTrue((Path(job["backup"]) / "obsolete-resource").exists())
        self.assertFalse((self.dest / "obsolete-resource").exists())
        self.assertTrue((self.dest / "new-resource").exists())
        self.assertEqual((data / "save").read_text(), "career")
        self.assertTrue(Path(job["result"]).read_text().startswith("success:"))

    def test_failure_after_swap_restores_and_restarts_old_launcher(self):
        _, _, job = self.prepare(fail_at_install=True)
        with self.assertRaisesRegex(RuntimeError, "startup check"):
            update_helper.apply_archive(job)
        self.assertEqual(self.wait_marker(), "old")
        self.assertFalse((self.dest / "new-resource").exists())
        self.assertFalse(Path(job["backup"]).exists())
        self.assertIn("restored", Path(job["result"]).read_text())

    def test_failure_moving_old_copy_reopens_unchanged_launcher(self):
        _, _, job = self.prepare()
        rename = Path.rename
        def fail_old(path, target):
            if path == self.dest:
                raise PermissionError("install directory became read-only")
            return rename(path, target)
        with patch.object(Path, "rename", fail_old), self.assertRaises(PermissionError):
            update_helper.apply_archive(job)
        self.assertEqual(self.wait_marker(), "old")
        self.assertTrue((self.dest / "forza-launcher").exists())
        self.assertIn("kept", Path(job["result"]).read_text())

    def test_helper_start_failure_keeps_app_and_removes_stage(self):
        helper, job_file, job = self.prepare()
        helper.write_text("raise RuntimeError('helper failed before ready')\n")
        with patch.object(linux_update.paths, "FROZEN", False), \
                self.assertRaisesRegex(updater.UpdateError, "could not start"):
            linux_update.start_archive((helper, job_file, job))
        self.assertFalse(helper.parent.exists())
        self.assertTrue((self.dest / "forza-launcher").exists())

    def test_second_helper_cannot_update_locked_installation(self):
        import fcntl
        _, _, job = self.prepare()
        with open(self.dest.parent / ".forza-updater.lock", "a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(BlockingIOError):
                update_helper.apply_archive(job)
        self.assertFalse(Path(job["ready"]).exists())
        self.assertFalse(Path(job["backup"]).exists())

    def test_helper_waits_for_original_process_and_runs_outside_install(self):
        helper, job_file, job = self.prepare()
        parent = subprocess.Popen(["/bin/sh", "-c", "sleep 30"])
        job["parent_pid"] = parent.pid
        job_file.write_text(json.dumps(job))
        with patch.object(linux_update.paths, "FROZEN", False):
            process = linux_update.start_archive((helper, job_file, job))
        try:
            self.assertTrue(Path(job["ready"]).exists())
            self.assertFalse(Path(job["backup"]).exists())
            parent.terminate()
            parent.wait(timeout=3)
            self.assertEqual(process.wait(timeout=5), 0)
            self.assertEqual(self.wait_marker(), "new")
        finally:
            if parent.poll() is None:
                parent.terminate()
                parent.wait()
            if process.poll() is None:
                process.terminate()
                process.wait()

    def test_timeout_or_concurrent_replacement_keeps_installed_copy(self):
        _, _, job = self.prepare()
        job["parent_pid"] = os.getpid()
        with self.assertRaisesRegex(RuntimeError, "did not close"):
            update_helper.apply_archive(job, wait_seconds=0)
        self.assertFalse(Path(job["backup"]).exists())
        job["fingerprint"] = [0, 0]
        with self.assertRaisesRegex(RuntimeError, "Another update"):
            update_helper.apply_archive(job)
        self.assertTrue((self.dest / "forza-launcher").exists())

    def test_modified_download_or_user_data_in_app_is_refused(self):
        archive, asset = self.bundle()
        with self.assertRaisesRegex(updater.UpdateError, "user data"):
            linux_update.prepare_archive(archive, asset, self.version, self.dest, self.config,
                                         protected=[self.dest / "saves"])
        archive.write_bytes(b"modified")
        with self.assertRaisesRegex(updater.UpdateError, "changed"):
            linux_update.prepare_archive(archive, asset, self.version, self.dest, self.config)
        self.assertTrue((self.dest / "forza-launcher").exists())
        self.assertFalse(list(self.root.glob(".forza-update-*")))

    def test_cancel_insufficient_space_and_bad_executable_leave_no_stage(self):
        archive, asset = self.bundle()
        with self.assertRaises(fetch.Cancelled):
            linux_update.prepare_archive(archive, asset, self.version, self.dest, self.config, cancelled=lambda: True)
        with patch.object(linux_update.shutil, "disk_usage", return_value=type("Disk", (), {"free": 0})()), \
                self.assertRaisesRegex(updater.UpdateError, "free space"):
            linux_update.prepare_archive(archive, asset, self.version, self.dest, self.config)
        with self.assertRaisesRegex(RuntimeError, "startup check"):
            linux_update.prepare_archive(archive, asset, "9.9.9", self.dest, self.config)
        self.assertFalse(list(self.root.glob(".forza-update-*")))

    def test_traversal_and_outside_symlink_are_rejected(self):
        def extra(bundle):
            item = tarfile.TarInfo("ForzaHorizon1-ReImagined/../../escaped")
            item.size = 4
            bundle.addfile(item, io.BytesIO(b"evil"))
        archive, asset = self.bundle(extra=extra)
        with self.assertRaises(tarfile.FilterError):
            linux_update.prepare_archive(archive, asset, self.version, self.dest, self.config)
        self.assertFalse((self.root / "escaped").exists())
        self.assertFalse(list(self.root.glob(".forza-update-*")))
        with tarfile.open(archive, "w:gz") as bundle:
            link = tarfile.TarInfo("ForzaHorizon1-ReImagined/link")
            link.type = tarfile.SYMTYPE
            link.linkname = "../../outside"
            bundle.addfile(link)
        asset = updater.Asset(archive.name, "", hashlib.sha256(archive.read_bytes()).hexdigest(), archive.stat().st_size)
        with self.assertRaises(tarfile.FilterError):
            linux_update.prepare_archive(archive, asset, self.version, self.dest, self.config)
        self.assertFalse(list(self.root.glob(".forza-update-*")))


class FlatpakTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="flatpak update ")
        self.root = Path(self.temp.name).resolve()
        self.bundle = self.root / "update bundle.flatpak"
        self.bundle.write_bytes(b"bundle")
        self.asset = updater.Asset(self.bundle.name, "", hashlib.sha256(b"bundle").hexdigest(), 6)
        self.info = self.root / "flatpak-info"
        self.info.write_text("[Application]\nname=" + linux_update.APP_ID + "\n[Instance]\napp-path=/user/deployment/files\n")

    def tearDown(self):
        self.temp.cleanup()

    def test_bundle_updates_current_scope_and_restarts_without_interpolation(self):
        with patch.object(linux_update, "host_run", side_effect=["/user/deployment", "installed", ""]) as run, \
                patch.object(linux_update.paths, "config_dir", return_value=self.root):
            linux_update.install_flatpak(self.bundle, self.asset, self.info)
        calls = [call.args[0] for call in run.call_args_list]
        self.assertEqual(calls[0], ["flatpak", "info", "--user", "--show-location", linux_update.APP_ID])
        self.assertEqual(calls[1], ["flatpak", "install", "--user", "--bundle", "--or-update", "--noninteractive",
                                    "--assumeyes", str(self.bundle)])
        self.assertEqual(calls[2][-4:-1], ["--user", linux_update.APP_ID, str(self.root / "update-install.log")])
        self.assertEqual(Path(calls[2][-1]).parent, self.root)
        self.assertEqual(run.call_args.kwargs["watch_bus"], False)
        self.assertTrue((self.root / "update-result.txt").read_text().startswith("success:"))

    def test_restart_retry_uses_same_installation_after_deployment_changed(self):
        self.info.write_text("[Application]\nname=" + linux_update.APP_ID +
                             "\n[Instance]\napp-path=/user/app/master/old-commit/files\n")
        error = updater.UpdateError("Failed to install bundle: " + linux_update.APP_ID + " already installed")
        with patch.object(linux_update, "host_run", side_effect=["/user/app/master/new-commit", error, ""]) as run, \
                patch.object(linux_update.paths, "config_dir", return_value=self.root):
            linux_update.install_flatpak(self.bundle, self.asset, self.info)
        self.assertEqual(run.call_count, 3)
        self.assertEqual(run.call_args_list[1].kwargs["environment"], {"LC_ALL": "C"})
        self.assertIn("retrying launcher startup", (self.root / "update-install.log").read_text())

    def test_other_already_installed_errors_are_not_ignored(self):
        with patch.object(linux_update, "host_run", side_effect=["/user/deployment", updater.UpdateError("dependency already installed")]) as run, \
                patch.object(linux_update.paths, "config_dir", return_value=self.root), \
                self.assertRaises(updater.UpdateError):
            linux_update.install_flatpak(self.bundle, self.asset, self.info)
        self.assertEqual(run.call_count, 2)

    def test_failed_restart_after_successful_install_is_distinct_and_not_success(self):
        with patch.object(linux_update, "host_run", side_effect=["/user/deployment", "installed", updater.UpdateError("launch failed")]), \
                patch.object(linux_update.paths, "config_dir", return_value=self.root), \
                self.assertRaisesRegex(updater.UpdateError, "Update installed, but.*could not restart"):
            linux_update.install_flatpak(self.bundle, self.asset, self.info)
        self.assertFalse((self.root / "update-result.txt").exists())
        log = (self.root / "update-install.log").read_text()
        self.assertIn("installed", log)
        self.assertIn("could not restart", log)

    def test_restart_script_detects_child_failure(self):
        if os.name != "posix":
            self.skipTest("host restart uses a POSIX shell")
        fake = self.root / "flatpak"
        fake.write_text("#!/bin/sh\nexit 9\n")
        fake.chmod(0o755)
        result = subprocess.run(["/bin/sh", "-c", linux_update.FLATPAK_RESTART_SCRIPT,
                                 "restart-test", "--user", linux_update.APP_ID, str(self.root / "restart.log"), str(self.root / "update-ready-test")],
                                env={**os.environ, "PATH": str(self.root) + os.pathsep + os.environ["PATH"]}, timeout=5)
        self.assertEqual(result.returncode, 1)

    def test_restart_script_requires_gui_acknowledgement(self):
        if os.name != "posix":
            self.skipTest("host restart uses a POSIX shell")
        ready = self.root / "update-ready-test"
        fake = self.root / "flatpak"
        fake.write_text("#!/bin/sh\nfor argument do\ncase \"$argument\" in --env=FORZA_UPDATE_READY=*) touch \"${argument#--env=FORZA_UPDATE_READY=}\";; esac\ndone\nsleep 2\n")
        fake.chmod(0o755)
        result = subprocess.run(["/bin/sh", "-c", linux_update.FLATPAK_RESTART_SCRIPT, "restart-test", "--user",
                                 linux_update.APP_ID, str(self.root / "restart.log"), str(ready)],
                                env={**os.environ, "PATH": str(self.root) + os.pathsep + os.environ["PATH"]}, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertTrue(ready.exists())

    def test_restart_timeout_kills_only_the_replacement_instance(self):
        if os.name != "posix":
            self.skipTest("host restart uses a POSIX shell")
        ready = self.root / "update-ready-test"
        fake = self.root / "flatpak"
        fake.write_text('#!/bin/sh\nif [ "$1" = kill ]; then printf "%s" "$2" > "$KILL_RECORD"; exit 0; fi\nprintf "replacement-instance" >&3\nexec sleep 30\n')
        fake.chmod(0o755)
        # Shorten the polling window without changing timeout cleanup behavior.
        script = linux_update.FLATPAK_RESTART_SCRIPT.replace('"$attempt" -lt 60', '"$attempt" -lt 1')
        record = self.root / "killed-instance"
        result = subprocess.run(["/bin/sh", "-c", script, "restart-test", "--user", linux_update.APP_ID,
                                 str(self.root / "restart.log"), str(ready)],
                                env={**os.environ, "PATH": str(self.root) + os.pathsep + os.environ["PATH"],
                                     "KILL_RECORD": str(record)}, timeout=5)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(record.read_text(), "replacement-instance")
        self.assertFalse(Path(str(ready) + ".instance").exists())

    def test_gui_acknowledgement_only_writes_inside_launcher_configuration(self):
        ready = self.root / "update-ready-test"
        with patch.object(linux_update.paths, "config_dir", return_value=self.root), \
                patch.dict(os.environ, {"FORZA_UPDATE_READY": str(ready)}):
            self.assertTrue(linux_update.acknowledge_gui_ready())
        self.assertTrue(ready.exists())
        invalid = self.root.parent / "update-ready-outside"
        with patch.object(linux_update.paths, "config_dir", return_value=self.root), \
                patch.dict(os.environ, {"FORZA_UPDATE_READY": str(invalid)}):
            self.assertFalse(linux_update.acknowledge_gui_ready())
        self.assertFalse(invalid.exists())

    def test_restart_forwards_display_without_sandbox_bus_or_directory_paths(self):
        with patch.object(linux_update, "host_run", side_effect=["/user/deployment", "installed", ""]) as run, \
                patch.object(linux_update.paths, "config_dir", return_value=self.root), \
                patch.dict(os.environ, {"DISPLAY": ":99", "WAYLAND_DISPLAY": "wayland-2", "XDG_RUNTIME_DIR": "/sandbox"}):
            linux_update.install_flatpak(self.bundle, self.asset, self.info)
        self.assertEqual(run.call_args.kwargs["environment"], {"DISPLAY": ":99", "WAYLAND_DISPLAY": "wayland-2"})

    def test_host_transaction_is_bound_to_spawn_process_lifetime(self):
        with patch.object(linux_update.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "ok", "")) as run:
            self.assertEqual(linux_update.host_run(["flatpak", "info"]), "ok")
        self.assertEqual(run.call_args.args[0][:3], ["flatpak-spawn", "--host", "--watch-bus"])

    def test_system_scope_is_preserved(self):
        with patch.object(linux_update, "host_run", side_effect=["/other", "/user/deployment", "installed", ""]) as run, \
                patch.object(linux_update.paths, "config_dir", return_value=self.root):
            linux_update.install_flatpak(self.bundle, self.asset, self.info)
        self.assertIn("--system", run.call_args_list[2].args[0])

    def test_permissions_or_failed_install_do_not_restart(self):
        for responses, message in (([updater.UpdateError("denied"), updater.UpdateError("denied")], "host permission"),
                                   (["/user/deployment", updater.UpdateError("install failed")], "install failed")):
            with patch.object(linux_update, "host_run", side_effect=responses) as run, \
                    patch.object(linux_update.paths, "config_dir", return_value=self.root), \
                    self.assertRaisesRegex(updater.UpdateError, message):
                linux_update.install_flatpak(self.bundle, self.asset, self.info)
            self.assertEqual(run.call_count, 2)
            self.assertFalse((self.root / "update-result.txt").exists())


if __name__ == "__main__":
    unittest.main()
