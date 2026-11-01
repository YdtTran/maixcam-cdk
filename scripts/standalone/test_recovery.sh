#!/bin/sh
# Run on device via USB. Briefly restarts AP and app; never touches Bluetooth.
set -eu
DIR=/root/maixcam-tests/standalone-web
mkdir -p "$DIR"
LOG="$DIR/recovery-test.txt"
exec > "$LOG" 2>&1
date -u
old_media=$(pidof stream_webrtc-m2)
old_ap=$(cat /run/maixcam-hostapd.pid)
old_dns=$(cat /run/maixcam-dnsmasq.pid)
/etc/init.d/S99webrtc_autostart start
/root/maixcam_standalone_network.sh start
[ "$(pidof stream_webrtc-m2)" = "$old_media" ]
[ "$(cat /run/maixcam-hostapd.pid)" = "$old_ap" ]
[ "$(cat /run/maixcam-dnsmasq.pid)" = "$old_dns" ]
echo 'PASS: idempotent supervisor/network start; one media owner'
# Graceful daemon interruption, detected by existing supervisor at 10s cadence.
kill "$old_ap"
n=0
while :; do
    sleep 2
    n=$((n + 1))
    if /root/maixcam_standalone_network.sh status && [ "$(cat /run/maixcam-hostapd.pid)" != "$old_ap" ]; then break; fi
    [ "$n" -lt 20 ] || { echo 'FAIL: AP recovery timeout'; exit 1; }
done
[ "$(pidof stream_webrtc-m2)" = "$old_media" ]
echo "PASS: AP/DHCP/DNS recovery in at most $((n * 2))s; media process unchanged"
/root/webrtc_service.sh stop
n=0
while :; do
    sleep 2
    n=$((n + 1))
    new_media=$(pidof stream_webrtc-m2 2>/dev/null || true)
    if [ -n "$new_media" ] && [ "$new_media" != "$old_media" ]; then break; fi
    [ "$n" -lt 20 ] || { echo 'FAIL: media recovery timeout'; exit 1; }
done
case "$new_media" in *' '*) echo 'FAIL: multiple media owners'; exit 1 ;; esac
echo "PASS: media process restart in at most $((n * 2))s; PID=$new_media"
/root/maixcam_standalone_network.sh status
bluetoothctl info 6C:16:29:7B:00:D4 | head -12
