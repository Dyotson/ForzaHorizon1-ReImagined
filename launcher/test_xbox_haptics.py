"""Xbox settings, packet guards and the game-owned impulse trigger output path."""
import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from pathlib import Path
import socket
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher.haptics.settings import DEFAULTS, normalize
from forza_launcher.haptics.service import HapticsService, supports_xbox
from forza_launcher.haptics.telemetry import Decoder
from forza_launcher.haptics.xbox import ACK, COMMAND, XboxOutput, amplitudes
from test_haptics import packet, wait_for


class WireTests(unittest.TestCase):
    def test_amplitudes_are_independent_and_zero_for_off_or_inactive(self):
        frame = Decoder(1).decode(packet())
        prefs = {**DEFAULTS, 'xbox_enabled': True, 'xbox_intensity': 100, 'xbox_brake': 100, 'xbox_throttle': 25}
        self.assertEqual(amplitudes(frame, prefs), (65535, 16384))
        self.assertEqual(amplitudes(frame, {**prefs, 'xbox_brake': 0}), (0, 16384))
        self.assertEqual(amplitudes(frame, DEFAULTS), (0, 0))
        self.assertEqual(amplitudes(None, prefs), (0, 0))
        self.assertEqual(amplitudes(Decoder(1).decode(packet(active=0)), prefs), (0, 0))

    def test_ack_requires_nonce_and_a_sent_command_and_rejects_replay(self):
        output = XboxOutput(10, True)
        ack = lambda token=10, seq=0: ACK.pack(b'FH1A', 1, ACK.size, token, seq, 1)
        self.assertFalse(output.acknowledge(ack(), 1))
        output.sequence = 1
        self.assertFalse(output.acknowledge(ack(token=11), 1))
        self.assertFalse(output.acknowledge(ack(seq=1), 1))
        self.assertTrue(output.acknowledge(ack(), 1))
        self.assertFalse(output.acknowledge(ack(), 2))
        self.assertIn('active', output.status(True, True, 1))
        self.assertIn('Waiting', output.status(True, True, 3))

    def test_normalization_and_metadata_do_not_assume_old_bridge_supports_xbox(self):
        prefs = normalize({**DEFAULTS, 'xbox_enabled': 'yes', 'xbox_brake': 110, 'xbox_throttle': -3})
        self.assertFalse(prefs['xbox_enabled'])
        self.assertEqual((prefs['xbox_brake'], prefs['xbox_throttle']), (100, 0))
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)/'forza-telemetry.json'
            path.write_text('{"protocol":"FH1T","version":1}')
            self.assertFalse(supports_xbox(path.parent))
            path.write_text('{"protocol":"FH1T","version":1,"xbox_trigger_rumble":1}')
            self.assertTrue(supports_xbox(path.parent))


class XboxServiceTests(unittest.TestCase):
    def test_udp_commands_ack_status_timeout_disable_and_stop(self):
        service = HapticsService({**DEFAULTS, 'xbox_enabled': True}, xbox_capable=True,
                                 device_factory=lambda: self.fail('Xbox must not open DualSense HID'))
        env = service.new_stream()
        token = int(env['FORZA_TELEMETRY_TOKEN'], 16)
        receiver = ('127.0.0.1', int(env['FORZA_TELEMETRY_PORT']))
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as game:
            game.bind(('127.0.0.1', 0))
            game.settimeout(1)
            service.start()
            try:
                game.sendto(packet(token), receiver)
                data, _ = game.recvfrom(4096)
                cmd = COMMAND.unpack(data)
                self.assertEqual(cmd[:4], (b'FH1X', 1, 32, token))
                self.assertGreater(cmd[6], cmd[7])
                game.sendto(ACK.pack(b'FH1A', 1, 24, token, cmd[4], -2), receiver)
                wait_for(lambda: 'Update the launcher' in service.xbox_status)
                game.sendto(packet(token, seq=1), receiver)
                data, _ = game.recvfrom(4096)
                seq = COMMAND.unpack(data)[4]
                # Acknowledgements from a different socket cannot claim output success.
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stranger:
                    stranger.sendto(ACK.pack(b'FH1A', 1, 24, token, seq, 1), receiver)
                time.sleep(.03)
                self.assertNotIn('active', service.xbox_status)
                game.sendto(ACK.pack(b'FH1A', 1, 24, token, seq, 1), receiver)
                wait_for(lambda: 'active' in service.xbox_status)
                wait_for(lambda: 'released' in service.xbox_status)
                # Drain commands queued before the watchdog, then find the release.
                while True:
                    data, _ = game.recvfrom(4096)
                    if COMMAND.unpack(data)[6:8] == (0, 0): break
                game.sendto(packet(token, seq=2), receiver)
                wait_for(lambda: 'released' not in service.xbox_status)
                service.update(DEFAULTS)
                wait_for(lambda: service.xbox_status == 'Xbox trigger vibration off')
                while True:
                    data, _ = game.recvfrom(4096)
                    if COMMAND.unpack(data)[6:8] == (0, 0): break
            finally:
                service.stop()
                service.join()
        self.assertFalse(service.running)

    def test_old_runtime_gets_no_output_and_session_rotation_changes_token(self):
        service = HapticsService({**DEFAULTS, 'xbox_enabled': True})
        env = service.new_stream()
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as game:
            game.settimeout(.1)
            service.start()
            try:
                game.sendto(packet(int(env['FORZA_TELEMETRY_TOKEN'], 16)),
                            ('127.0.0.1', int(env['FORZA_TELEMETRY_PORT'])))
                wait_for(lambda: 'Rebuild' in service.xbox_status)
                with self.assertRaises(socket.timeout): game.recvfrom(4096)
                self.assertNotEqual(service.new_stream()['FORZA_TELEMETRY_TOKEN'], env['FORZA_TELEMETRY_TOKEN'])
            finally:
                service.stop()
                service.join()

    def test_controls_preserve_both_controller_preferences(self):
        from PySide6.QtWidgets import QApplication
        from forza_launcher.config import Config
        from forza_launcher.ui.controls_page import ControlsPage
        app = QApplication.instance() or QApplication([])
        with tempfile.TemporaryDirectory() as d:
            cfg = Config(Path(d)/'launcher.json')
            page = ControlsPage(cfg)
            page.haptics_enabled.setChecked(True)
            page.haptics_fields['brake_strength'].setValue(4)
            page.xbox_enabled.setChecked(True)
            page.xbox_fields['xbox_brake'].setValue(60)
            saved = Config(cfg.path).d['haptics']
            self.assertTrue(saved['enabled'])
            self.assertTrue(saved['xbox_enabled'])
            self.assertEqual(saved['brake_strength'], 4)
            self.assertEqual(saved['xbox_brake'], 60)
            page.close()
            page.deleteLater()
            app.processEvents()


if __name__ == '__main__': unittest.main()
