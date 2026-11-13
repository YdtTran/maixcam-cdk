#!/usr/bin/env python3
"""
Non-invasive per-process, per-thread, interrupt, and /api/perf_stats profiler for MaixCAM.
"""
import argparse
import http.client
import json
import socket
import ssl
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def ssh_run(target: str, cmd: str, timeout: int = 20) -> str:
    res = subprocess.run(
        ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", target, cmd],
        capture_output=True, text=True, timeout=timeout, check=True
    )
    return res.stdout


def fetch_https_json(host_ip: str, path: str, cafile: Path | None) -> dict | None:
    try:
        if cafile and cafile.is_file():
            ctx = ssl.create_default_context(cafile=str(cafile))
            ctx.check_hostname = False
            conn = http.client.HTTPSConnection(host_ip, 8000, timeout=3, context=ctx)
        else:
            ctx = ssl._create_unverified_context()
            conn = http.client.HTTPSConnection(host_ip, 8000, timeout=3, context=ctx)
        conn.request("GET", path)
        resp = conn.getresponse()
        if resp.status != 200:
            conn.close()
            return None
        data = json.loads(resp.read().decode())
        conn.close()
        return data
    except Exception:
        return None


REMOTE_SNAP_CMD = r'''
cat /proc/uptime
head -n 3 /proc/stat
PID=$(pidof stream_webrtc-m2 | awk '{print $1}')
echo "MAIN_PID=$PID"
if [ -n "$PID" ]; then
  for t in /proc/$PID/task/*; do
    tid=$(basename "$t")
    comm=$(cat "$t/comm" 2>/dev/null || echo "?")
    stat=$(cat "$t/stat" 2>/dev/null || continue)
    vcsw=$(awk '/^voluntary_ctxt_switches:/{print $2}' "$t/status" 2>/dev/null || echo 0)
    nvcsw=$(awk '/^nonvoluntary_ctxt_switches:/{print $2}' "$t/status" 2>/dev/null || echo 0)
    echo "THR $tid $vcsw $nvcsw [$comm] $stat"
  done
fi
for p in /proc/[0-9]*/stat; do
  pid=${p#/proc/}; pid=${pid%/stat}
  [ -r "$p" ] && echo "PROC $pid $(cat "$p" 2>/dev/null || true)"
done
grep -E "^ *(5|26|27|32|33|37|41):" /proc/interrupts || true
'''


def parse_snapshot(raw: str) -> dict:
    snap = {"uptime": 0.0, "idle_up": 0.0, "cpu": [], "threads": {}, "procs": {}, "irqs": {}, "pid": None}
    for line in raw.splitlines():
        if not line:
            continue
        if snap["uptime"] == 0.0 and line[0].isdigit():
            parts = line.split()
            snap["uptime"] = float(parts[0])
            snap["idle_up"] = float(parts[1])
        elif line.startswith("cpu "):
            snap["cpu"] = [int(x) for x in line.split()[1:]]
        elif line.startswith("MAIN_PID="):
            snap["pid"] = line.split("=", 1)[1].strip()
        elif line.startswith("THR "):
            # THR <tid> <vcsw> <nvcsw> [<comm>] <stat>
            rb = line.find("]")
            lb = line.find("[")
            hdr = line[:lb].split()
            tid = int(hdr[1])
            vcsw = int(hdr[2])
            nvcsw = int(hdr[3])
            comm = line[lb + 1:rb]
            stat_part = line[rb + 2:]
            rp = stat_part.rfind(")")
            if rp < 0:
                continue
            fields = stat_part[rp + 2:].split()
            if len(fields) < 17:
                continue
            snap["threads"][tid] = {
                "comm": comm,
                "state": fields[0],
                "minflt": int(fields[7]),
                "utime": int(fields[11]),
                "stime": int(fields[12]),
                "prio": int(fields[15]),
                "nice": int(fields[16]),
                "policy": int(fields[38]) if len(fields) > 38 else 0,
                "vcsw": vcsw,
                "nvcsw": nvcsw,
            }
        elif line.startswith("PROC "):
            rest = line[5:]
            sp = rest.find(" ")
            pid = int(rest[:sp])
            lp = rest.find("(")
            rp = rest.rfind(")")
            if lp < 0 or rp < 0:
                continue
            comm = rest[lp + 1:rp]
            fields = rest[rp + 2:].split()
            snap["procs"][pid] = {
                "comm": comm,
                "utime": int(fields[11]),
                "stime": int(fields[12]),
            }
        elif ":" in line and line.lstrip()[0].isdigit():
            parts = line.split()
            irq = parts[0].rstrip(":")
            cnt = int(parts[1])
            name = " ".join(parts[3:])
            snap["irqs"][irq] = (cnt, name)
    return snap


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="root@10.172.16.1")
    ap.add_argument("--host-ip", default="10.172.16.1")
    ap.add_argument("--duration", type=float, default=5.0)
    ap.add_argument("--label", default="baseline")
    args = ap.parse_args()

    cafile = ROOT / "build/standalone-secrets/ca.crt"
    if not cafile.is_file():
        cafile = None

    p1 = fetch_https_json(args.host_ip, "/api/perf_stats", cafile)
    a1 = fetch_https_json(args.host_ip, "/api/audio_stats", cafile)
    s1 = parse_snapshot(ssh_run(args.target, REMOTE_SNAP_CMD))
    time.sleep(args.duration)
    s2 = parse_snapshot(ssh_run(args.target, REMOTE_SNAP_CMD))
    p2 = fetch_https_json(args.host_ip, "/api/perf_stats", cafile)
    a2 = fetch_https_json(args.host_ip, "/api/audio_stats", cafile)

    dt = s2["uptime"] - s1["uptime"]
    if dt <= 0:
        dt = args.duration

    print(f"=== PERF PROBE [{args.label}] window={dt:.2f}s (PID={s2['pid']}) ===")
    c1, c2 = s1["cpu"], s2["cpu"]
    dc = [b - a for a, b in zip(c1, c2)]
    tot = sum(dc)
    if tot > 0:
        usr = 100.0 * (dc[0] + dc[1]) / tot
        sys_p = 100.0 * dc[2] / tot
        idle = 100.0 * dc[3] / tot
        iow = 100.0 * dc[4] / tot
        irq = 100.0 * dc[5] / tot
        sirq = 100.0 * dc[6] / tot
        print(f"SYS CPU: busy={100.0 - idle:.1f}% (usr={usr:.1f}% sys={sys_p:.1f}% irq={irq:.1f}% sirq={sirq:.1f}% iow={iow:.1f}% idle={idle:.1f}%)")

    # Top processes
    proc_rows = []
    for pid, cur in s2["procs"].items():
        prev = s1["procs"].get(pid)
        if not prev:
            continue
        du = cur["utime"] - prev["utime"]
        ds = cur["stime"] - prev["stime"]
        if du + ds > 0:
            proc_rows.append((100.0 * (du + ds) / (dt * 100.0), 100.0 * du / (dt * 100.0), 100.0 * ds / (dt * 100.0), pid, cur["comm"]))
    proc_rows.sort(reverse=True)
    print("\nTop Processes:")
    for pct, u, s, pid, comm in proc_rows[:10]:
        print(f"  PID={pid:<5} {comm:<18} CPU={pct:5.1f}% (usr={u:4.1f}% sys={s:4.1f}%)")

    # Threads
    thr_rows = []
    for tid, cur in s2["threads"].items():
        prev = s1["threads"].get(tid)
        if not prev:
            continue
        du = cur["utime"] - prev["utime"]
        ds = cur["stime"] - prev["stime"]
        dv = (cur["vcsw"] - prev["vcsw"]) / dt
        dnv = (cur["nvcsw"] - prev["nvcsw"]) / dt
        dmf = (cur["minflt"] - prev["minflt"]) / dt
        pct = 100.0 * (du + ds) / (dt * 100.0)
        thr_rows.append((pct, 100.0 * du / (dt * 100.0), 100.0 * ds / (dt * 100.0), tid, cur["comm"], dv, dnv, dmf, cur["prio"], cur["policy"]))
    thr_rows.sort(reverse=True)
    print("\nStream WebRTC Threads:")
    for pct, u, s, tid, comm, dv, dnv, dmf, prio, pol in thr_rows:
        if pct >= 0.05 or dv + dnv >= 10:
            print(f"  TID={tid:<5} {comm:<16} CPU={pct:5.1f}% (u={u:4.1f}% s={s:4.1f}%) vcsw={dv:6.0f}/s nvcsw={dnv:6.0f}/s minflt={dmf:6.0f}/s prio={prio} pol={pol}")

    print("\nInterrupts/sec:")
    for irq, (c_end, name) in sorted(s2["irqs"].items(), key=lambda x: int(x[0])):
        c_beg = s1["irqs"].get(irq, (c_end, name))[0]
        print(f"  IRQ {irq:<3} ({name}): {(c_end - c_beg) / dt:8.1f}/s")

    if a1 and a2:
        up1, up2 = a1.get("uplink", {}), a2.get("uplink", {})
        dn1, dn2 = a1.get("downlink") or {}, a2.get("downlink") or {}
        pb1, pb2 = dn1.get("playback") or {}, dn2.get("playback") or {}
        print("\nAudio Rates:")
        if up1 and up2:
            print(f"  Uplink: sent={(up2.get('packets_sent',0)-up1.get('packets_sent',0))/dt:.1f} pkt/s, failed={(up2.get('packets_failed',0)-up1.get('packets_failed',0))/dt:.1f}/s")
        if dn1 and dn2:
            print(f"  Downlink: talk={dn2.get('talk_active')} rx={(dn2.get('packets_received',0)-dn1.get('packets_received',0))/dt:.1f} pkt/s, dec={(dn2.get('packets_decoded',0)-dn1.get('packets_decoded',0))/dt:.1f} pkt/s, err={dn2.get('decode_errors',0)-dn1.get('decode_errors',0)}")
        if pb1 and pb2:
            print(f"  Playback: frames={(pb2.get('frames_written',0)-pb1.get('frames_written',0))/dt:.1f}/s, underruns={pb2.get('underruns',0)-pb1.get('underruns',0)}, alsa_err={pb2.get('alsa_errors',0)-pb1.get('alsa_errors',0)}, open={pb2.get('is_device_open')}")

    if p1 and p2:
        p_dt = dt
        if a1 and a2 and a2.get("timestamp_ms", 0) > a1.get("timestamp_ms", 0):
            p_dt = (a2["timestamp_ms"] - a1["timestamp_ms"]) / 1000.0
        print(f"\nPerf Instrumentation Deltas (p_dt={p_dt:.2f}s):")
        for sec in ("video", "mic", "rx", "playback", "mutex"):
            d1, d2 = p1.get(sec, {}), p2.get(sec, {})
            if not d1 or not d2:
                continue
            diffs = {k: (d2[k] - d1[k]) for k in d2 if isinstance(d2[k], (int, float)) and k in d1}
            if sec == "video" and diffs.get("loop_iters", 0) > 0:
                iters = diffs["loop_iters"]
                frames = diffs.get("frames_sent", 0)
                print(f"  video: iters={iters/p_dt:.1f}/s vi_pops={diffs.get('vi_pops',0)/p_dt:.1f}/s vi_empty={diffs.get('vi_empty',0)/p_dt:.1f}/s venc_pops={diffs.get('venc_pops',0)/p_dt:.1f}/s venc_empty={diffs.get('venc_empty',0)/p_dt:.1f}/s sent={frames/p_dt:.1f}fps cpu={diffs.get('thread_cpu_ns',0)/(p_dt*1e7):.1f}%")
                print(f"    avg_per_iter_us: vi_pop={diffs.get('vi_pop_ns',0)/(iters*1e3):.1f} venc_pop={diffs.get('venc_pop_ns',0)/(iters*1e3):.1f} | per_frame_us: copy={diffs.get('copy_ns',0)/(max(1,frames)*1e3):.1f} webrtc_push={diffs.get('webrtc_push_ns',0)/(max(1,frames)*1e3):.1f} venc_push={diffs.get('venc_push_ns',0)/(max(1,diffs.get('vi_pops',1))*1e3):.1f}")
            elif sec == "mic":
                reads = max(1, diffs.get("read_calls", 0))
                enc = max(1, diffs.get("frames_encoded", 0))
                print(f"  mic: reads={diffs.get('read_calls',0)/p_dt:.1f}/s empty={diffs.get('empty_reads',0)/p_dt:.1f}/s encoded={diffs.get('frames_encoded',0)/p_dt:.1f}/s cpu={diffs.get('thread_cpu_ns',0)/(p_dt*1e7):.1f}%")
                print(f"    avg_us: read={diffs.get('read_ns',0)/(reads*1e3):.1f} opus_enc={diffs.get('opus_encode_ns',0)/(enc*1e3):.1f} webrtc_push={diffs.get('webrtc_push_ns',0)/(enc*1e3):.1f}")
            elif sec == "rx":
                dec = max(1, diffs.get("packets_decoded", 0))
                print(f"  rx: decoded={diffs.get('packets_decoded',0)/p_dt:.1f}/s cpu={diffs.get('thread_cpu_ns',0)/(p_dt*1e7):.1f}% avg_decode_us={diffs.get('opus_decode_ns',0)/(dec*1e3):.1f}")
            elif sec == "playback":
                wr = max(1, diffs.get("write_calls", 0))
                print(f"  playback: write_calls={diffs.get('write_calls',0)/p_dt:.1f}/s eagain={diffs.get('eagain_count',0)} partial={diffs.get('partial_count',0)} zero={diffs.get('zero_count',0)} cpu={diffs.get('thread_cpu_ns',0)/(p_dt*1e7):.1f}% avg_write_us={diffs.get('alsa_write_ns',0)/(wr*1e3):.1f}")
            elif sec == "mutex":
                lk = max(1, diffs.get("media_locks", 0))
                print(f"  _media_mutex: locks={diffs.get('media_locks',0)/p_dt:.1f}/s total_wait_ms={diffs.get('media_wait_ns',0)/1e6:.2f} total_hold_ms={diffs.get('media_hold_ns',0)/1e6:.2f} avg_hold_us={diffs.get('media_hold_ns',0)/(lk*1e3):.1f}")


if __name__ == "__main__":
    main()
