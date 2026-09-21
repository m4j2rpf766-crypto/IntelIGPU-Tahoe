import pathlib
import tempfile
import json
import signal
import unittest
from unittest.mock import patch
import desktop_handoff as handoff


def state(pid=205, power=0, count=None):
    return dict(pid=pid, identity=str(pid), power=power, submitted=count,
                completed=count, pending=False)


class HandoffTests(unittest.TestCase):
    def exercise(self, samples, marker=None):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            with patch.object(handoff, 'MARKER', root / 'marker'), patch.object(
                handoff, 'snapshot', side_effect=samples
            ), patch.object(handoff.time, 'sleep'), patch.object(handoff.os, 'kill') as kill:
                if marker:
                    handoff.MARKER.write_text(marker)
                try:
                    result = handoff.ensure_desktop(lambda _: b'boot1', None, None, root)
                except RuntimeError:
                    self.assertEqual(kill.call_count, 0)
                    raise
                return result, kill.call_args_list

    def test_active_desktop_is_not_restarted(self):
        _, calls = self.exercise([state(300, 2, 10), state(300, 2, 20)])
        self.assertFalse(calls)

    def test_cold_desktop_restarts_once_and_requires_new_flips(self):
        result, calls = self.exercise([state(), state(), state(), state(300, 2, 1), state(300, 2, 5)])
        self.assertEqual(result['pid'], 300)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0].args[0], 205)

    def test_same_boot_retry_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, 'already attempted'):
            self.exercise([state(), state()], '{"boot":"boot1"}')

    def test_active_stall_is_not_restarted(self):
        with self.assertRaisesRegex(RuntimeError, 'not progressing'):
            self.exercise([state(300, 2, 10), state(300, 2, 10)])

    def test_changing_windowserver_is_not_signaled(self):
        with self.assertRaisesRegex(RuntimeError, 'changed during'):
            self.exercise([state(), state(300)])

    def test_incoherent_counters_are_not_success(self):
        before, after = state(300, 2, 10), state(300, 2, 20)
        after['submitted'] = 19
        self.assertFalse(handoff.progressing(before, after))

    def test_legacy_timeout_escalates_only_original_pid_once(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'failure.json').write_text(json.dumps(dict(error='Desktop handoff did not produce completed flips within 120 seconds; no retry')))
            marker = root / 'marker'
            marker.write_text(json.dumps(dict(boot='boot1', before=state(), evidence=str(root))))
            samples = [state()] * 5 + [state(300, 2, 1), state(300, 2, 10)]
            with patch.object(handoff, 'MARKER', marker), patch.object(handoff, 'snapshot', side_effect=samples), patch.object(handoff.time, 'sleep'), patch.object(handoff.os, 'kill') as kill:
                handoff.ensure_desktop(lambda *a, **k: b'boot1', None, None, root)
                kill.assert_called_once_with(205, signal.SIGKILL)
                self.assertTrue((root / 'windowserver-before-force.sample.txt').exists())
                self.assertEqual(json.loads(marker.read_text())['phase'], 'verified')

    def test_force_intent_refuses_repeated_kill(self):
        with self.assertRaisesRegex(RuntimeError, 'already attempted'):
            self.exercise([state(), state()], json.dumps(dict(boot='boot1', phase='force-intent', before=state())))

    def test_force_does_not_target_replacement_during_sampling(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(handoff.os, 'kill') as kill:
            samples = iter([state(), state(300)])
            with self.assertRaisesRegex(RuntimeError, 'changed during'):
                handoff.force_stuck_exit(lambda *a, **k: b'stack', lambda: next(samples), dict(before=state()), pathlib.Path(directory))
            kill.assert_not_called()

    def test_term_timeout_forces_once_then_accepts_new_desktop(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            samples = [state()] * 6 + [state(300, 2, 1), state(300, 2, 10)]
            with patch.object(handoff, 'MARKER', root / 'marker'), patch.object(handoff, 'snapshot', side_effect=samples), patch.object(handoff.time, 'sleep'), patch.object(handoff.time, 'monotonic', side_effect=[0, 0, 16, 16, 19, 22]), patch.object(handoff.os, 'kill') as kill:
                handoff.ensure_desktop(lambda *a, **k: b'boot1', None, None, root)
                self.assertEqual([call.args for call in kill.call_args_list], [(205, signal.SIGTERM), (205, signal.SIGKILL)])


if __name__ == '__main__':
    unittest.main()
