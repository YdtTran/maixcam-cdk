#!/bin/sh
set -eu
DIR=/root/maixcam-tests/standalone-web
exec > "$DIR/bluetooth-recovery.txt" 2>&1
date -u
old=$(pidof stream_webrtc-m2)
bluetoothctl info 6C:16:29:7B:00:D4 | head -12
bluetoothctl disconnect 6C:16:29:7B:00:D4
n=0
while :; do
    sleep 1
    n=$((n + 1))
    if bluetoothctl info 6C:16:29:7B:00:D4 | grep -q 'Connected: yes'; then break; fi
    [ "$n" -lt 35 ] || { echo 'FAIL: headset did not reconnect in 35 seconds'; exit 1; }
done
[ "$(pidof stream_webrtc-m2)" = "$old" ]
/root/maixcam_standalone_network.sh status
echo "PASS: existing Bluetooth manager reconnected in at most ${n}s; AP and media PID unchanged"
bluealsa-aplay -L
cat /run/bt_manager.state
