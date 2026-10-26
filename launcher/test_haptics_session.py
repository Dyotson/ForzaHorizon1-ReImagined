"""Launch-time opt-in, telemetry isolation and haptics settings on relaunch."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher.game import LaunchPlan, Session
from forza_launcher.haptics.settings import DEFAULTS


def game_call(child):
    # the Popen patch is on the shared subprocess module: take the game's call only
    return [c for c in child.call_args_list if c.args and c.args[0] == ['test-game']][-1]


class SessionHapticsTests(unittest.TestCase):
    def test_telemetry_is_only_advertised_when_an_effect_is_enabled(self):
        for settings, wanted in (({}, False), (DEFAULTS, False),
                                 ({**DEFAULTS, 'enabled': True}, True),
                                 ({**DEFAULTS, 'xbox_enabled': True}, True)):
            with self.subTest(settings=settings), tempfile.TemporaryDirectory() as d:
                root = Path(d)
                plan = LaunchPlan(None, root, ['test-game'],
                                  {'FORZA_TELEMETRY_PORT': 'stale', 'FORZA_TELEMETRY_TOKEN': 'stale'},
                                  [], root/'forza.toml', '', haptics_settings=dict(settings))
                session = Session(plan)
                service = Mock()
                stream = {'FORZA_TELEMETRY_PORT': '12345', 'FORZA_TELEMETRY_TOKEN': 'fresh'}
                service.new_stream.return_value = stream
                with patch('forza_launcher.haptics.service.supported', return_value=True) as supported, \
                     patch('forza_launcher.haptics.service.supports_xbox', return_value=True), \
                     patch('forza_launcher.haptics.service.HapticsService', return_value=service) as factory, \
                     patch('forza_launcher.game.subprocess.Popen') as child:
                    session.start()
                    env = game_call(child).kwargs['env']
                    if wanted:
                        factory.assert_called_once_with(settings, xbox_capable=True)
                        service.start.assert_called_once()
                        for key, value in stream.items():
                            self.assertEqual(env[key], value)
                    else:
                        factory.assert_not_called()
                        supported.assert_not_called()
                        self.assertIsNone(session.haptics)
                        for key in stream:
                            self.assertNotIn(key, env)

    def test_controls_changes_apply_live_and_to_the_next_process(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            plan = LaunchPlan(None, root, ['test-game'], {}, [], root/'forza.toml', '',
                              haptics_settings={**DEFAULTS, 'enabled': True})
            session = Session(plan)
            service = Mock()
            service.new_stream.return_value = {'FORZA_TELEMETRY_PORT': '12345',
                                               'FORZA_TELEMETRY_TOKEN': 'fresh'}
            with patch('forza_launcher.haptics.service.supported', return_value=True), \
                 patch('forza_launcher.haptics.service.supports_xbox', return_value=True), \
                 patch('forza_launcher.haptics.service.HapticsService', return_value=service) as factory, \
                 patch('forza_launcher.game.subprocess.Popen') as child:
                session.start()
                session.update_haptics(DEFAULTS)
                service.update.assert_called_once_with(DEFAULTS)
                self.assertTrue(session.relaunch())
                service.stop.assert_called_once()
                self.assertIsNone(session.haptics)
                self.assertNotIn('FORZA_TELEMETRY_PORT', game_call(child).kwargs['env'])
                session.update_haptics({**DEFAULTS, 'xbox_enabled': True})
                self.assertIsNone(session.haptics)  # Running child has no telemetry destination.
                self.assertEqual(factory.call_count, 1)
                self.assertTrue(session.relaunch())
                self.assertEqual(factory.call_count, 2)
                self.assertEqual(game_call(child).kwargs['env']['FORZA_TELEMETRY_PORT'], '12345')


if __name__ == '__main__':
    unittest.main()
