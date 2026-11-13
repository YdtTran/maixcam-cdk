#!/usr/bin/env python3
"""30-minute sustained full-duplex hardware stability sampler for MaixCAM-CDK."""
import argparse
import csv
import json
import ssl
import subprocess
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "docs/performance-investigation/phase-2/data/stability_30m.csv"

SSL_CTX = ssl.create_default_context()
SSL_CTX.check_hostname = False
SSL_CTX.verify_mode = ssl.CERT_NONE


def fetch_json(url: str) -> dict:
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req, context=SSL_CTX, timeout=5) as resp:
        return json.loads(resp.read().decode("utf-8"))


def ssh_sample(target: str) -> dict:
    cmd = (
        "head -n 1 /proc/stat; "
        "grep -E '^(MemTotal|MemAvailable):' /proc/meminfo; "
        "cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0; "
        "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0; "
        "pid=$(pidof stream_webrtc-m2 || echo 0); echo PID=$pid; "
        "if [ \"$pid\" != 0 ]; then grep VmRSS /proc/$pid/status 2>/dev/null || echo 'VmRSS: 0 kB'; else echo 'VmRSS: 0 kB'; fi; "
        "dmesg | grep -c 'aic_btsdio' || true; "
        "dmesg | grep -cE 'cmd timed-out|bt send no cfm|OOM|Out of memory' || true"
    )
    out = subprocess.check_output(
        ["ssh", "-o", "ConnectTimeout=5", target, cmd],
        text=True,
        timeout=10,
    ).strip().splitlines()
    cpu_parts = [int(x) for x in out[0].split()[1:8]]
    mem_avail_kb = int(out[2].split()[1])
    temp_c = int(out[3]) / 1000.0 if int(out[3]) > 1000 else float(out[3])
    freq_mhz = int(out[4]) / 1000.0 if int(out[4]) > 1000 else float(out[4])
    pid = int(out[5].split("=")[1])
    rss_kb = int(out[6].split()[1])
    btsdio_cnt = int(out[7])
    err_cnt = int(out[8])
    return {
        "cpu_parts": cpu_parts,
        "mem_avail_kb": mem_avail_kb,
        "temp_c": temp_c,
        "freq_mhz": freq_mhz,
        "pid": pid,
        "rss_kb": rss_kb,
        "btsdio_cnt": btsdio_cnt,
        "err_cnt": err_cnt,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="root@10.172.16.1")
    ap.add_argument("--url", default="https://10.172.16.1:8000/")
    ap.add_argument("--duration", type=int, default=1800)
    ap.add_argument("--interval", type=int, default=10)
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    args = ap.parse_args()

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    client_dur = args.duration + 20
    client = subprocess.Popen(
        ["node", str(ROOT / "scripts/headless_client.mjs"),
         "--url", args.url, "--talk", "--listen", "--duration", str(client_dur)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    time.sleep(8)

    prev_sys = ssh_sample(args.target)
    prev_perf = fetch_json(f"{args.url.rstrip('/')}/api/perf_stats")
    prev_audio = fetch_json(f"{args.url.rstrip('/')}/api/audio_stats")
    t0 = time.monotonic()
    prev_t = t0

    fields = [
        "elapsed_s", "pid", "sys_cpu_pct", "fps", "uplink_pkt_s", "downlink_rx_s",
        "downlink_dec_s", "underruns", "alsa_err", "decode_err", "mem_avail_mb",
        "rss_mb", "temp_c", "freq_mhz", "btsdio_dmesg", "err_dmesg",
    ]
    rows = []
    with out_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        n_samples = args.duration // args.interval
        for i in range(1, n_samples + 1):
            next_t = t0 + i * args.interval
            sleep_s = next_t - time.monotonic()
            if sleep_s > 0:
                time.sleep(sleep_s)
            now = time.monotonic()
            dt = max(now - prev_t, 0.1)
            cur_sys = ssh_sample(args.target)
            cur_perf = fetch_json(f"{args.url.rstrip('/')}/api/perf_stats")
            cur_audio = fetch_json(f"{args.url.rstrip('/')}/api/audio_stats")

            d_cpu = [b - a for a, b in zip(prev_sys["cpu_parts"], cur_sys["cpu_parts"])]
            tot = max(sum(d_cpu), 1)
            busy_pct = 100.0 * (tot - d_cpu[3] - d_cpu[4]) / tot

            v_sent = cur_perf["video"]["frames_sent"] - prev_perf["video"]["frames_sent"]
            u_sent = cur_audio["uplink"]["packets_sent"] - prev_audio["uplink"]["packets_sent"]
            d_rx = cur_audio["downlink"]["packets_received"] - prev_audio["downlink"]["packets_received"]
            d_dec = cur_audio["downlink"]["packets_decoded"] - prev_audio["downlink"]["packets_decoded"]
            pb = cur_audio["downlink"]["playback"]

            row = {
                "elapsed_s": round(now - t0, 1),
                "pid": cur_sys["pid"],
                "sys_cpu_pct": round(busy_pct, 1),
                "fps": round(v_sent / dt, 2),
                "uplink_pkt_s": round(u_sent / dt, 1),
                "downlink_rx_s": round(d_rx / dt, 1),
                "downlink_dec_s": round(d_dec / dt, 1),
                "underruns": pb["underruns"],
                "alsa_err": pb["alsa_errors"],
                "decode_err": cur_audio["downlink"]["decode_errors"],
                "mem_avail_mb": round(cur_sys["mem_avail_kb"] / 1024.0, 2),
                "rss_mb": round(cur_sys["rss_kb"] / 1024.0, 2),
                "temp_c": round(cur_sys["temp_c"], 1),
                "freq_mhz": round(cur_sys["freq_mhz"], 1),
                "btsdio_dmesg": cur_sys["btsdio_cnt"],
                "err_dmesg": cur_sys["err_cnt"],
            }
            w.writerow(row)
            f.flush()
            rows.append(row)
            if i % 6 == 0 or i == 1 or i == n_samples:
                print(
                    f"[{row['elapsed_s']:6.1f}s] cpu={row['sys_cpu_pct']:4.1f}% "
                    f"fps={row['fps']:5.2f} up={row['uplink_pkt_s']:4.1f} "
                    f"dn={row['downlink_dec_s']:4.1f} rss={row['rss_mb']:5.2f}MB "
                    f"avail={row['mem_avail_mb']:5.2f}MB temp={row['temp_c']:4.1f}C "
                    f"ur={row['underruns']} err={row['err_dmesg']}",
                    flush=True,
                )
            prev_sys, prev_perf, prev_audio, prev_t = cur_sys, cur_perf, cur_audio, now

    client_out, _ = client.communicate(timeout=30)
    print("=== CLIENT SUMMARY ===")
    print(client_out.strip())

    time.sleep(3)
    reconn = subprocess.check_output(
        ["node", str(ROOT / "scripts/headless_client.mjs"),
         "--url", args.url, "--talk", "--listen", "--duration", "15"],
        text=True, timeout=45,
    )
    print("=== RECONNECT CHECK ===")
    print(reconn.strip())


if __name__ == "__main__":
    main()
