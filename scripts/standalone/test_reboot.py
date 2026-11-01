#!/usr/bin/env python3
"""Reboot through USB and retain honest post-boot readiness evidence."""
import json
import subprocess
import time
from pathlib import Path

TARGET = 'root@10.172.16.1'
OUT = Path('agent_progress/standalone/evidence/reboot.json')
def ssh(command, timeout=10, check=True):
    result = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=3', TARGET, command],
                            capture_output=True, text=True, timeout=timeout)
    if check and result.returncode:
        raise RuntimeError(result.stderr)
    return result.stdout

before = ssh('cat /proc/sys/kernel/random/boot_id').strip()
print('BOOT_ID_BEFORE=' + before, flush=True)
ssh('sync; reboot', check=False)
started = time.monotonic()
report = {'boot_id_before': before, 'result': 'BLOCKED'}
for attempt in range(45):
    time.sleep(2)
    try:
        after = ssh('cat /proc/sys/kernel/random/boot_id; cat /run/webrtc_autostart.state; '
                    '/root/maixcam_standalone_network.sh status; pidof stream_webrtc-m2', timeout=6)
        lines = after.splitlines()
        if lines and lines[0] != before and 'STATE=READY' in after:
            report.update(result='PASS', boot_id_after=lines[0],
                          observed_ready_after_seconds=round(time.monotonic() - started, 2),
                          readiness=after,
                          diagnostics=ssh('sh /root/maixcam-tests/standalone-web/diagnose.sh', timeout=15))
            break
    except (RuntimeError, subprocess.SubprocessError):
        pass
OUT.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: value for key, value in report.items() if key != 'diagnostics'}, indent=2), flush=True)
if report['result'] != 'PASS':
    raise SystemExit('USB/AP readiness not restored: operator hard reset required; stop device-dependent work')
