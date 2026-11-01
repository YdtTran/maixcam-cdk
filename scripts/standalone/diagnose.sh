#!/bin/sh
# Persistent snapshot; no scans, discovery or media-device opens.
set -eu
DEST=/root/maixcam-tests/standalone-web
mkdir -p "$DEST"
REPORT="$DEST/diagnose-$(date -u +%Y%m%d-%H%M%S).txt"
{
    date -u
    uname -a
    cat /proc/sys/kernel/random/boot_id
    cat /proc/uptime
    ip -4 addr
    ip route
    iw dev wlan0 info
    iw dev wlan0 station dump
    hostapd_cli -p /run/maixcam-hostapd -i wlan0 status
    cat /run/webrtc_autostart.state
    netstat -tln
    free -m
    ps w
    for name in stream_webrtc-m2 hostapd dnsmasq bluealsa bluetoothd; do
        for p in $(pidof "$name" 2>/dev/null || true); do
            echo "PROCESS=$name PID=$p"
            cat "/proc/$p/status"
            cat "/proc/$p/stat"
            if [ -r "/proc/$p/smaps_rollup" ]; then cat "/proc/$p/smaps_rollup"; fi
        done
    done
    cat /proc/stat
    sleep 5
    cat /proc/uptime
    for p in $(pidof stream_webrtc-m2 hostapd dnsmasq bluealsa bluetoothd 2>/dev/null || true); do
        cat "/proc/$p/stat" 2>/dev/null || true
    done
    cat /proc/stat
    bluetoothctl info 6C:16:29:7B:00:D4
    cat "$DEST/dhcp.leases" 2>/dev/null || true
    tail -n 35 "$DEST/hostapd.log"
    tail -n 35 "$DEST/supervisor.log"
} > "$REPORT" 2>&1
cat "$REPORT"
echo "REPORT=$REPORT"
