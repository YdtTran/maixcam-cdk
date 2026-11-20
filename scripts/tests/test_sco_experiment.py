import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

scripts = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(scripts))
spec = importlib.util.spec_from_file_location('sco_experiment', scripts / 'sco_experiment.py')
experiment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(experiment)


class ExperimentSafetyTest(unittest.TestCase):
    def test_watchdog_terminates_stuck_parent_before_recovery(self):
        parent = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
        try:
            start = experiment.identity(parent.pid)
            with tempfile.TemporaryDirectory() as directory:
                recovered = Path(directory) / 'called'
                with mock.patch.object(experiment, 'restore', side_effect=lambda *_: recovered.touch()), \
                     mock.patch.object(experiment.shutil, 'rmtree'):
                    experiment.watchdog_recover(parent.pid, start, Path(directory), [],
                                                '/unused', {}, '/unused', False, delay=0)
                parent.wait(timeout=3)
                self.assertEqual(parent.returncode, -signal.SIGTERM)
                self.assertTrue(recovered.exists())
        finally:
            if parent.poll() is None:
                parent.terminate()
            parent.wait(timeout=3)

    @unittest.skipUnless(os.environ.get('DBUS_SESSION_BUS_ADDRESS'), 'requires a private session bus')
    def test_real_private_dbus_owner_is_replaced_before_media_restore(self):
        service_code = (
            "from gi.repository import Gio,GLib; "
            "b=Gio.bus_get_sync(Gio.BusType.SESSION,None); "
            "b.call_sync('org.freedesktop.DBus','/org/freedesktop/DBus',"
            "'org.freedesktop.DBus','RequestName',"
            "GLib.Variant('(su)',('org.bluealsa',0)),None,Gio.DBusCallFlags.NONE,-1,None); "
            "GLib.MainLoop().run()"
        )
        with tempfile.TemporaryDirectory() as directory:
            env = os.environ.copy()
            env['SCO_DIAG_RUN'] = directory
            probe = subprocess.Popen([sys.executable, '-c', service_code], env=env)
            replacements = []
            real_run = subprocess.run

            def fake_pids(name):
                if name != 'bluealsa':
                    return [999998]
                return [process.pid for process in [probe] + replacements if experiment.identity(process.pid)]

            def fake_env(pid):
                return ({'SCO_DIAG_RUN': directory} if pid == probe.pid else
                        {'LD_PRELOAD': '/tmp/libg4c4_sco_write_align.so'})

            def run_or_start(command, **kwargs):
                if command == [experiment.BLUEALSA_SERVICE, 'start']:
                    clean_env = os.environ.copy()
                    clean_env.pop('SCO_DIAG_RUN', None)
                    replacements.append(subprocess.Popen([sys.executable, '-c', service_code], env=clean_env))
                    return mock.Mock(returncode=0)
                return real_run(command, **kwargs)

            try:
                with mock.patch.dict(os.environ, {'DBUS_SYSTEM_BUS_ADDRESS': os.environ['DBUS_SESSION_BUS_ADDRESS']}), \
                     mock.patch.object(experiment, 'pids', side_effect=fake_pids), \
                     mock.patch.object(experiment, 'process_env', side_effect=fake_env), \
                     mock.patch.object(experiment.subprocess, 'run', side_effect=run_or_start):
                    self.assertTrue(experiment.wait_for(experiment.bluealsa_dbus_present, 5))
                    self.assertFalse(experiment.bluealsa_ready())
                    self.assertTrue(experiment.restore(Path(directory), [], '/unused', {}, '/unused', True))
                    self.assertTrue(experiment.bluealsa_ready())
                    self.assertIsNotNone(probe.poll())
            finally:
                for process in [probe] + replacements:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=3)

    def test_restore_stops_real_probe_and_resumes_real_supervisor(self):
        with tempfile.TemporaryDirectory() as directory:
            env = os.environ.copy()
            env['SCO_DIAG_RUN'] = directory
            probe = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'], env=env)
            supervisor = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
            try:
                start = experiment.identity(supervisor.pid)
                self.assertIsNotNone(start)
                os.kill(supervisor.pid, signal.SIGSTOP)
                state = {'started': False}
                def fake_pids(name):
                    if name == 'bluealsa':
                        return [999999] if state['started'] else ([probe.pid] if experiment.identity(probe.pid) else [])
                    return [999998]
                def start_service(*_args, **_kwargs):
                    state['started'] = True
                    return mock.Mock(returncode=0)
                with mock.patch.object(experiment, 'pids', side_effect=fake_pids), \
                     mock.patch.object(experiment, 'bluealsa_ready', side_effect=lambda: state['started']), \
                     mock.patch.object(experiment, 'bluealsa_dbus_present', return_value=False), \
                     mock.patch.object(experiment.subprocess, 'run', side_effect=start_service):
                    self.assertTrue(experiment.restore(Path(directory), [(supervisor.pid, start)],
                                                       '/unused', {}, '/unused', True))
                probe.wait(timeout=3)
                self.assertTrue(state['started'])
                self.assertEqual(experiment.identity(supervisor.pid), start)
                self.assertTrue(experiment.wait_for(
                    lambda: '\nState:\tT' not in Path('/proc', str(supervisor.pid), 'status').read_text(), 2))
            finally:
                for process in (probe, supervisor):
                    if process.poll() is None:
                        os.kill(process.pid, signal.SIGCONT)
                        process.terminate()
                    process.wait(timeout=3)

    def test_requires_sco_playback_pcm(self):
        a2dp = 'bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=a2dp\n    EDIFIER, playback\n'
        capture = 'bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=sco\n    EDIFIER, capture\n'
        playback = 'bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=sco\n    EDIFIER, playback\n'
        self.assertFalse(experiment.sco_playback_ready(a2dp + capture))
        self.assertTrue(experiment.sco_playback_ready(a2dp + capture + playback))

    def test_temporary_probe_is_not_restored_bluealsa(self):
        with mock.patch.object(experiment, 'pids', return_value=[123]), \
             mock.patch.object(experiment, 'process_env', return_value={'SCO_DIAG_RUN': '/tmp/probe'}):
            self.assertFalse(experiment.bluealsa_ready())

    def test_rejects_wrong_fixture_before_device_operations(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / 'wrong.wav'
            fixture.write_bytes(b'wrong recording')
            with self.assertRaises(ValueError):
                experiment.validate_fixture(fixture)

    def test_supervisor_resumed_if_bluetooth_restore_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(experiment, 'pids', return_value=[]), \
                 mock.patch.object(experiment, 'identity', return_value='start'), \
                 mock.patch.object(experiment.subprocess, 'run', side_effect=RuntimeError('failure')), \
                 mock.patch.object(experiment.os, 'kill') as kill:
                with self.assertRaises(RuntimeError):
                    experiment.restore(Path(directory), [(123, 'start')], '/media', {}, '/tmp', True)
                kill.assert_called_once_with(123, experiment.signal.SIGCONT)
                self.assertFalse((Path(directory) / 'restored').exists())

    def test_waits_for_old_dbus_owner_before_starting_daemon(self):
        with tempfile.TemporaryDirectory() as directory:
            state = {'started': False, 'checks': 0}

            def owner_exists():
                state['checks'] += 1
                return state['checks'] < 3

            def fake_pids(name):
                return ([123] if state['started'] else []) if name == 'bluealsa' else [456]

            def start_service(*_args, **_kwargs):
                self.assertGreaterEqual(state['checks'], 3)
                state['started'] = True
                return mock.Mock(returncode=0)

            with mock.patch.object(experiment, 'pids', side_effect=fake_pids), \
                 mock.patch.object(experiment, 'bluealsa_ready', side_effect=lambda: state['started']), \
                 mock.patch.object(experiment, 'bluealsa_dbus_present', side_effect=owner_exists), \
                 mock.patch.object(experiment.subprocess, 'run', side_effect=start_service):
                self.assertTrue(experiment.restore(Path(directory), [], '/media', {}, '/tmp', True))


if __name__ == '__main__':
    unittest.main()
