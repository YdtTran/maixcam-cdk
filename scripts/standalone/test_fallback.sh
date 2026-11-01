#!/bin/sh
# Bounded hardware fallback trial. Run only through USB with no active operator.
set -eu
DIR=/root/maixcam-tests/standalone-web
CONFIG=/etc/maixcam-standalone/hostapd.conf
exec > "$DIR/fallback-test.txt" 2>&1
cp -p "$CONFIG" "$DIR/hostapd-before-fallback.conf"
media=$(pidof stream_webrtc-m2)
if [ -s /run/webrtc_autostart.pid ]; then
    p=$(cat /run/webrtc_autostart.pid)
    kill "$p"
    n=0
    while kill -0 "$p" 2>/dev/null && [ "$n" -lt 6 ]; do sleep 1; n=$((n + 1)); done
    ! kill -0 "$p" 2>/dev/null || { echo 'FAIL: supervisor did not stop'; exit 1; }
fi
restore() {
    cp -p "$DIR/hostapd-before-fallback.conf" "$CONFIG"
    /root/maixcam_standalone_network.sh restart
    /etc/init.d/S99webrtc_autostart start
}
trap restore EXIT INT TERM
# Fail before RF startup; no ACS/channel scan, no invalid regulatory channel.
sed -i 's/^interface=wlan0$/interface=missing_ap_test/' "$CONFIG"
/root/maixcam_standalone_network.sh restart
hostapd_cli -p /run/maixcam-hostapd -i wlan0 status | grep '^freq=2412$'
[ "$(pidof stream_webrtc-m2)" = "$media" ]
bluetoothctl info 6C:16:29:7B:00:D4 | grep 'Connected: yes'
echo 'PASS: primary startup failure selected 2.4 GHz channel 1 fallback; media PID unchanged and Bluetooth connected'
