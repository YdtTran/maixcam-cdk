#!/usr/bin/env python3
"""Regression tests for MaixCAM Bluetooth SCO/HFP duplex silence repair.

Covers:
1. Startup ordering & SCO MTU 120:4 preparation in target_services/webrtc_service.sh
   (including resetting an early-boot 255:4 eSCO link before opening SCO sockets).
2. Idempotent reconnection behavior in connect_bluetooth_mic() (zero ConnectProfile
   or bluetoothctl connect churn when SCO PCM is already registered).
3. Capture lifecycle parameters (2000 ms stall threshold, 1500 ms backoff, 20 ms/80 ms ALSA timing).
4. Playback scheduling (no startup pre-warm, 20 ms/80 ms ALSA timing, 20 ms silence pacing
   during active Talk gaps <= 500 ms, close_device() on Talk OFF / >500 ms idle).
"""

from pathlib import Path
import os
import stat
import subprocess
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
WEBRTC_SERVICE_SH = REPO_ROOT / "target_services" / "webrtc_service.sh"
MAIX_WEBRTC_CPP = REPO_ROOT / "src" / "maix_webrtc_maixcam.cpp"
BT_CAPTURE_HPP = REPO_ROOT / "src" / "g4c4_bt_capture.hpp"
BT_PLAYBACK_HPP = REPO_ROOT / "src" / "g4c4_bt_playback.hpp"
AUDIO_DSP_HPP = REPO_ROOT / "src" / "maix_webrtc_audio_dsp.hpp"


def _write_executable(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


class TestScoDuplexRepair(unittest.TestCase):
    def _extract_shell_functions(self) -> str:
        lines = WEBRTC_SERVICE_SH.read_text(encoding="utf-8").splitlines()
        body = []
        for line in lines:
            if line.startswith('case "$1" in'):
                break
            body.append(line)
        return "\n".join(body) + "\n"

    def _make_mock_env(self, tmp: Path, *, sco_mtu: str, has_esco: bool, has_sco_pcm: bool, connected: bool):
        bin_dir = tmp / "bin"
        bin_dir.mkdir()
        log_file = tmp / "calls.log"
        state_dir = tmp / "state"
        state_dir.mkdir()
        (state_dir / "sco_mtu").write_text(sco_mtu, encoding="utf-8")
        (state_dir / "has_esco").write_text("1" if has_esco else "0", encoding="utf-8")
        (state_dir / "has_sco_pcm").write_text("1" if has_sco_pcm else "0", encoding="utf-8")
        (state_dir / "connected").write_text("1" if connected else "0", encoding="utf-8")

        _write_executable(
            bin_dir / "sleep",
            "#!/bin/sh\nexit 0\n",
        )
        _write_executable(
            bin_dir / "hciconfig",
            f"""#!/bin/sh
echo "hciconfig $*" >> "{log_file}"
if [ "$1" = "-a" ]; then
    mtu=$(cat "{state_dir}/sco_mtu")
    echo "hci0: Type: Primary  Bus: SDIO"
    echo "    UP RUNNING PSCAN"
    echo "    ACL MTU: 1021:9  SCO MTU: $mtu"
    exit 0
fi
if [ "$2" = "scomtu" ]; then
    echo "$3" > "{state_dir}/sco_mtu"
fi
exit 0
""",
        )
        _write_executable(
            bin_dir / "hcitool",
            f"""#!/bin/sh
echo "hcitool $*" >> "{log_file}"
if [ "$(cat "{state_dir}/has_esco")" = "1" ]; then
    echo "Connections:"
    echo "    < eSCO 6C:16:29:7B:00:D4 handle 257 state 1 lm CENTRAL"
else
    echo "Connections:"
fi
exit 0
""",
        )
        _write_executable(
            bin_dir / "bluealsa-aplay",
            f"""#!/bin/sh
echo "bluealsa-aplay $*" >> "{log_file}"
if [ "$(cat "{state_dir}/has_sco_pcm")" = "1" ]; then
    echo "bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=sco,SRV=org.bluealsa"
fi
exit 0
""",
        )
        _write_executable(
            bin_dir / "bluetoothctl",
            f"""#!/bin/sh
echo "bluetoothctl $*" >> "{log_file}"
if [ "$1" = "info" ]; then
    if [ "$(cat "{state_dir}/connected")" = "1" ]; then
        echo "Device 6C:16:29:7B:00:D4"
        echo "    Connected: yes"
    else
        echo "Device 6C:16:29:7B:00:D4"
        echo "    Connected: no"
    fi
elif [ "$1" = "disconnect" ]; then
    echo "0" > "{state_dir}/connected"
    echo "0" > "{state_dir}/has_esco"
    echo "0" > "{state_dir}/has_sco_pcm"
elif [ "$1" = "connect" ]; then
    echo "1" > "{state_dir}/connected"
fi
exit 0
""",
        )
        _write_executable(
            bin_dir / "dbus-send",
            f"""#!/bin/sh
echo "dbus-send $*" >> "{log_file}"
echo "1" > "{state_dir}/has_sco_pcm"
exit 0
""",
        )
        return bin_dir, log_file, state_dir

    def test_prepare_bluetooth_controller_resets_255_esco_and_sets_120_4(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            bin_dir, log_file, state_dir = self._make_mock_env(
                tmp, sco_mtu="255:4", has_esco=True, has_sco_pcm=True, connected=True
            )
            runner = tmp / "run.sh"
            runner.write_text(
                self._extract_shell_functions()
                + "\nprepare_bluetooth_controller\n",
                encoding="utf-8",
            )
            env = os.environ.copy()
            env["PATH"] = f"{bin_dir}:{env.get('PATH', '')}"
            subprocess.run(["/bin/sh", str(runner)], env=env, check=True)

            calls = log_file.read_text(encoding="utf-8").splitlines()
            self.assertEqual((state_dir / "sco_mtu").read_text(encoding="utf-8").strip(), "120:4")
            self.assertIn("bluetoothctl disconnect 6C:16:29:7B:00:D4", calls)
            self.assertIn("hciconfig hci0 scomtu 120:4", calls)
            disconnect_idx = calls.index("bluetoothctl disconnect 6C:16:29:7B:00:D4")
            scomtu_idx = calls.index("hciconfig hci0 scomtu 120:4")
            self.assertLess(disconnect_idx, scomtu_idx)

    def test_connect_bluetooth_mic_is_idempotent_when_sco_pcm_exists(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            bin_dir, log_file, _ = self._make_mock_env(
                tmp, sco_mtu="120:4", has_esco=True, has_sco_pcm=True, connected=True
            )
            runner = tmp / "run.sh"
            runner.write_text(
                self._extract_shell_functions()
                + "\nconnect_bluetooth_mic\n",
                encoding="utf-8",
            )
            env = os.environ.copy()
            env["PATH"] = f"{bin_dir}:{env.get('PATH', '')}"
            subprocess.run(["/bin/sh", str(runner)], env=env, check=True)

            calls = log_file.read_text(encoding="utf-8").splitlines()
            self.assertEqual(calls, ["bluealsa-aplay -L"])

    def test_connect_bluetooth_mic_reconnects_and_requests_hfp_when_missing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            bin_dir, log_file, state_dir = self._make_mock_env(
                tmp, sco_mtu="255:4", has_esco=False, has_sco_pcm=False, connected=False
            )
            runner = tmp / "run.sh"
            runner.write_text(
                self._extract_shell_functions()
                + "\nconnect_bluetooth_mic\n",
                encoding="utf-8",
            )
            env = os.environ.copy()
            env["PATH"] = f"{bin_dir}:{env.get('PATH', '')}"
            subprocess.run(["/bin/sh", str(runner)], env=env, check=True)

            calls = log_file.read_text(encoding="utf-8")
            self.assertEqual((state_dir / "sco_mtu").read_text(encoding="utf-8").strip(), "120:4")
            self.assertIn("hciconfig hci0 scomtu 120:4", calls)
            self.assertIn("bluetoothctl connect 6C:16:29:7B:00:D4", calls)
            self.assertIn("org.bluez.Device1.ConnectProfile", calls)

    def test_capture_and_playback_contracts(self):
        cpp_text = MAIX_WEBRTC_CPP.read_text(encoding="utf-8")
        cap_text = BT_CAPTURE_HPP.read_text(encoding="utf-8")
        pb_text = BT_PLAYBACK_HPP.read_text(encoding="utf-8")
        dsp_text = AUDIO_DSP_HPP.read_text(encoding="utf-8")

        self.assertIn("milliseconds(2000)", cpp_text)
        self.assertIn("milliseconds(1500)", cpp_text)

        self.assertIn("unsigned int period_us = 20000;", cap_text)
        self.assertIn("unsigned int buffer_us = 80000;", cap_text)
        self.assertIn("PERIODIC_FLUSH_MS = 5000;", cap_text)

        self.assertIn("unsigned int period_us = 20000;", pb_text)
        self.assertIn("unsigned int buffer_us = 80000;", pb_text)
        self.assertIn("FLUSH_KEEP_FRAMES = 2;", pb_text)
        self.assertIn("PERIODIC_FLUSH_MS = 5000;", pb_text)
        self.assertIn("chunk.samples.assign(sample_rate_ / 50 * channels_, 0);", pb_text)
        self.assertIn("notify_stream_idle()", pb_text)
        self.assertIn("close_device();", pb_text)

        self.assertIn("PERIODIC_FLUSH_MS = 5000;", dsp_text)


if __name__ == "__main__":
    unittest.main()
