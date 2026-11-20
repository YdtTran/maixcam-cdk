#!/usr/bin/env python3
"""Run a bounded SCO comparison on MaixCAM; never changes service files."""

import argparse
import collections
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wave

from sco_trace_summary import summarize


FIXTURE_HASH = "3c60551adbb588d27ca59f5b225c14963f33253cc1f162b42ddf0e73b086c23c"
DEVICE = "bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=sco"
SERVICE = "/root/webrtc_service.sh"
BLUEALSA_SERVICE = "/etc/init.d/S41bluealsa"
LOCK = "/run/maix-sco-experiment.lock"


def identity(pid):
    try:
        stat = Path('/proc', str(pid), 'stat').read_text()
        fields = stat[stat.rfind(')') + 2:].split()
        return None if fields[0] == 'Z' else fields[19]
    except (FileNotFoundError, ProcessLookupError):
        return None


def pids(name):
    result = subprocess.run(['pidof', name], capture_output=True, text=True)
    return [int(pid) for pid in result.stdout.split()] if result.returncode == 0 else []


def validate_fixture(path):
    if hashlib.sha256(path.read_bytes()).hexdigest() != FIXTURE_HASH:
        raise ValueError('Fixture hash does not match the comparison recording')
    with wave.open(str(path), 'rb') as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()) != (1, 2, 8000, 120000):
            raise ValueError('Fixture must be 15 seconds of 8 kHz mono S16 PCM')


def wait_for(predicate, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(0.2)
    return False


def sco_playback_ready(output):
    lines = output.splitlines()
    return any(DEVICE in line and index + 1 < len(lines) and 'playback' in lines[index + 1]
               for index, line in enumerate(lines))


def process_env(pid):
    env = {}
    for item in Path('/proc', str(pid), 'environ').read_bytes().split(b'\0'):
        if b'=' in item:
            key, value = item.split(b'=', 1)
            env[os.fsdecode(key)] = os.fsdecode(value)
    return env


def bluealsa_dbus_present():
    return subprocess.run(['dbus-send', '--system', '--print-reply', '--reply-timeout=2000',
                           '--dest=org.bluealsa', '/org/bluealsa', 'org.freedesktop.DBus.Peer.Ping'],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3).returncode == 0


def bluealsa_ready():
    ids = pids('bluealsa')
    if len(ids) != 1:
        return False
    try:
        env = process_env(ids[0])
    except FileNotFoundError:
        return False
    if 'SCO_DIAG_RUN' in env or 'libg4c4_sco_write_align.so' not in env.get('LD_PRELOAD', ''):
        return False
    return bluealsa_dbus_present()


def stop_child(path):
    try:
        pid, start = path.read_text().split()
        pid = int(pid)
        if identity(pid) == start:
            os.kill(pid, signal.SIGTERM)
            if not wait_for(lambda: identity(pid) != start, 3):
                os.kill(pid, signal.SIGKILL)
    except (FileNotFoundError, ProcessLookupError):
        pass


def track(directory, name, process):
    start = identity(process.pid)
    if start is None:
        raise RuntimeError(name + ' exited during startup')
    (directory / name).write_text(str(process.pid) + ' ' + start)


def restore(directory, supervisors, media, media_env, media_cwd, restore_bluetooth):
    # Both parent and watchdog can restore; the lock and marker serialize them.
    with (directory / 'restore.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if (directory / 'restored').exists():
            return True
        marker = b'SCO_DIAG_RUN=' + os.fsencode(directory)
        marked = []
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit():
                continue
            try:
                if marker in (proc / 'environ').read_bytes().split(b'\0'):
                    pid = int(proc.name)
                    start = identity(pid)
                    if start is not None:
                        marked.append((pid, start))
                        os.kill(pid, signal.SIGTERM)
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                pass
        for name in ('player', 'trace', 'probe'):
            stop_child(directory / name)
        for pid, start in marked:
            if not wait_for(lambda: identity(pid) != start, 3):
                os.kill(pid, signal.SIGKILL)
        # Unlink even when subsequent service recovery fails; capture may contain microphone audio.
        (directory / 'trace.btsnoop').unlink(missing_ok=True)
        try:
            if restore_bluetooth:
                wait_for(lambda: not pids('bluealsa') or bluealsa_ready(), 5)
            if restore_bluetooth and not pids('bluealsa'):
                if not wait_for(lambda: not bluealsa_dbus_present(), 5):
                    raise RuntimeError('Temporary BlueALSA still owns its D-Bus name')
                subprocess.run([BLUEALSA_SERVICE, 'start'], timeout=15, check=True,
                               stdout=subprocess.DEVNULL)
            if not wait_for(bluealsa_ready, 10):
                raise RuntimeError('BlueALSA was not restored')
            time.sleep(1)
            if not bluealsa_ready():
                raise RuntimeError('BlueALSA did not remain available')
            if not pids('stream_webrtc-m2'):
                with (Path(media_cwd) / 'm2_runtime.log').open('ab') as log:
                    process = subprocess.Popen([media], cwd=media_cwd, env=media_env,
                                               stdin=subprocess.DEVNULL, stdout=log,
                                               stderr=subprocess.STDOUT, start_new_session=True)
                Path(media_cwd, 'm2.pid').write_text(str(process.pid) + '\n')
                watch = subprocess.Popen([SERVICE, 'bt-watch', str(process.pid)],
                                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                         stderr=subprocess.DEVNULL, start_new_session=True)
                Path(media_cwd, 'bt_profile_watch.pid').write_text(str(watch.pid) + '\n')
        finally:
            for pid, start in supervisors:
                if identity(pid) == start:
                    os.kill(pid, signal.SIGCONT)
        if not wait_for(lambda: bool(pids('stream_webrtc-m2')), 5):
            raise RuntimeError('Media process was not restored')
        (directory / 'restored').touch()
        return True


def watchdog_recover(parent_pid, parent_start, directory, supervisors, media,
                     media_env, media_cwd, restore_bluetooth, delay=90):
    time.sleep(delay)
    if identity(parent_pid) == parent_start:
        os.kill(parent_pid, signal.SIGTERM)
        if not wait_for(lambda: identity(parent_pid) != parent_start, 3):
            os.kill(parent_pid, signal.SIGKILL)
            if not wait_for(lambda: identity(parent_pid) != parent_start, 3):
                raise RuntimeError('Experiment parent could not be stopped')
    restore(directory, supervisors, media, media_env, media_cwd, restore_bluetooth)
    shutil.rmtree(directory, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--mode', choices=('baseline', 'restart-control', 'chunk'), required=True)
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--chunk', type=int, default=120)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise RuntimeError('Run as root on the device')
    validate_fixture(args.fixture)
    if args.mode == 'chunk' and (not args.probe or not args.probe.is_file() or
                                 args.chunk < 2 or args.chunk > 254 or args.chunk % 2):
        raise ValueError('A probe and an even measured chunk in [2,254] are required')
    if args.output.exists():
        raise ValueError('Refusing to overwrite an existing result')
    with open(LOCK, 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        run(args)


def run(args):
    media_ids, bluealsa_ids = pids('stream_webrtc-m2'), pids('bluealsa')
    if len(media_ids) != 1 or len(bluealsa_ids) != 1:
        raise RuntimeError('Expected exactly one media process and one BlueALSA process')
    media_pid, bluealsa_pid = media_ids[0], bluealsa_ids[0]
    media = os.readlink('/proc/%d/exe' % media_pid)
    media_cwd = os.readlink('/proc/%d/cwd' % media_pid)
    media_env = process_env(media_pid)
    bluealsa_env = process_env(bluealsa_pid)
    alignment = bluealsa_env.get('LD_PRELOAD', '')
    if not alignment or not Path(alignment).is_file():
        raise RuntimeError('Existing BlueALSA alignment preload not found')
    supervisors = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            if b'/root/webrtc_autostart_daemon.sh' in (proc / 'cmdline').read_bytes().split(b'\0'):
                supervisors.append((int(proc.name), identity(int(proc.name))))
        except FileNotFoundError:
            continue
    if not supervisors or any(start is None for _, start in supervisors):
        raise RuntimeError('Supervisor identity unavailable')

    directory = Path(tempfile.mkdtemp(prefix='maix-sco-experiment-', dir='/tmp'))
    os.chmod(directory, 0o700)
    restore_bluetooth = args.mode != 'baseline'
    parent_pid = os.getpid()
    parent_start = identity(parent_pid)
    watchdog = os.fork()
    if watchdog == 0:
        try:
            os.setsid()
            watchdog_recover(parent_pid, parent_start, directory, supervisors, media,
                             media_env, media_cwd, restore_bluetooth)
        finally:
            os._exit(0)
    result = {'mode': args.mode, 'fixture_sha256': FIXTURE_HASH, 'audibility': 'NOT VERIFIED'}
    child_env = os.environ.copy()
    child_env['SCO_DIAG_RUN'] = str(directory)
    restored = False
    try:
        for pid, start in supervisors:
            if identity(pid) != start:
                raise RuntimeError('Supervisor changed before pause')
            os.kill(pid, signal.SIGSTOP)
        subprocess.run([SERVICE, 'stop'], check=True, timeout=15, stdout=subprocess.DEVNULL)
        if pids('stream_webrtc-m2'):
            raise RuntimeError('Media process did not stop')
        if args.mode != 'baseline':
            subprocess.run([BLUEALSA_SERVICE, 'stop'], check=True, timeout=15,
                           stdout=subprocess.DEVNULL)
            if not wait_for(lambda: not pids('bluealsa'), 5):
                raise RuntimeError('Original BlueALSA did not stop')
            probe_env = bluealsa_env.copy()
            probe_env['SCO_DIAG_RUN'] = str(directory)
            if args.mode == 'chunk':
                probe_env.update({'LD_PRELOAD': str(args.probe.resolve()) + ':' + alignment,
                                  'SCO_DIAG_CHUNK': str(args.chunk),
                                  'SCO_DIAG_LOG': str(directory / 'writes.log')})
            probe = subprocess.Popen(['/usr/bin/bluealsa', '-p', 'a2dp-source', '-p', 'hfp-ag', '-S'],
                                     env=probe_env, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                     start_new_session=True)
            track(directory, 'probe', probe)
            if not wait_for(lambda: probe.poll() is None and sco_playback_ready(
                            subprocess.run(['bluealsa-aplay', '-L'], capture_output=True, text=True,
                                           timeout=3).stdout), 20):
                raise RuntimeError('Temporary BlueALSA PCM not ready')
        trace_path = directory / 'trace.btsnoop'
        trace = subprocess.Popen(['btmon', '-w', str(trace_path)], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, start_new_session=True, env=child_env)
        track(directory, 'trace', trace)
        time.sleep(1)
        print('PLAYBACK START: listen now', flush=True)
        player = subprocess.Popen(['aplay', '-v', '-D', DEVICE, str(args.fixture)],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, start_new_session=True, env=child_env)
        track(directory, 'player', player)
        try:
            output, _ = player.communicate(timeout=22)
        except subprocess.TimeoutExpired:
            player.kill()
            output, _ = player.communicate()
            result['playback_timeout'] = True
        result.update({'aplay_exit': player.returncode, 'aplay_log': output[-4000:]})
        stop_child(directory / 'trace')
        decoded = subprocess.run(['btmon', '-r', str(trace_path)], capture_output=True,
                                 text=True, timeout=15, check=True)
        result['hci'] = summarize(decoded.stdout.splitlines())
        result['hci_setup'] = [line.strip() for line in decoded.stdout.splitlines()
                               if re.search(r'Transmission interval:|Retransmission window:|'
                                            r'RX packet length:|TX packet length:|Air mode:', line)]
        del decoded
        trace_path.unlink(missing_ok=True)
        log = directory / 'writes.log'
        if log.exists():
            writes = [line.split() for line in log.read_text().splitlines()]
            result['write_results'] = dict(collections.Counter(' '.join(row[2:]) for row in writes))
            timestamps = [float(row[0]) for row in writes]
            gaps = [(b - a) * 1000 for a, b in zip(timestamps, timestamps[1:]) if b >= a]
            result['write_gap_ms'] = {'median': sorted(gaps)[len(gaps) // 2], 'max': max(gaps)} if gaps else None
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as output_file:
            output_file.write(json.dumps(result, indent=2) + '\n')
        print('RESULT', args.output, 'APLAY_EXIT', result['aplay_exit'], flush=True)
    finally:
        try:
            restored = restore(directory, supervisors, media, media_env, media_cwd, restore_bluetooth)
        finally:
            if restored:
                os.kill(watchdog, signal.SIGTERM)
                os.waitpid(watchdog, 0)
                shutil.rmtree(directory, ignore_errors=True)
        print('RESTORED', restored, flush=True)


if __name__ == '__main__':
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        print('EXPERIMENT ERROR:', error, file=sys.stderr)
        sys.exit(1)
