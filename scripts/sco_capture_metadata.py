#!/usr/bin/env python3
"""Capture bounded HCI metadata; delete the temporary microphone-containing trace."""

import argparse
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

from sco_trace_summary import summarize


def interrupt(_signum, _frame):
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=20, choices=range(1, 31))
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, interrupt)

    with tempfile.TemporaryDirectory(prefix="sco-metadata-") as directory:
        os.chmod(directory, 0o700)
        trace = Path(directory) / "capture.btsnoop"
        capture = subprocess.Popen(["btmon", "-w", str(trace)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            time.sleep(args.seconds)
        finally:
            if capture.poll() is None:
                capture.terminate()
            try:
                _, error = capture.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                capture.kill()
                _, error = capture.communicate()
        if capture.returncode not in (0, -15) or not trace.exists():
            raise RuntimeError("btmon capture failed: " + error.decode(errors="replace"))

        decode = subprocess.Popen(["btmon", "-r", str(trace)], stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        try:
            output, error = decode.communicate(timeout=10)
            if decode.returncode != 0:
                raise RuntimeError("btmon decode failed: " + error)
            result = summarize(io.StringIO(output))
        finally:
            if decode.poll() is None:
                decode.kill()
                decode.wait()
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
