#!/bin/sh
set -u
PIDFILE=/run/webrtc_autostart.pid
STATE=/run/webrtc_autostart.state
LOG=/root/maixcam-tests/standalone-web/supervisor.log
NETWORK=/root/maixcam_standalone_network.sh
case "${1:-daemon}" in
    --status) cat "$STATE" 2>/dev/null; exit ;;
    --check-wifi) "$NETWORK" status; exit ;;
    daemon|--daemon) ;;
    *) exit 2 ;;
esac
[ -s "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null && exit 0
# mkdir provides a boot-scoped atomic launch lock, independent of PID timing.
mkdir /run/maixcam-supervisor.lock 2>/dev/null || exit 1
echo $$ > "$PIDFILE"
mkdir -p /root/maixcam-tests/standalone-web
cleanup() {
    rm -f "$PIDFILE"
    rmdir /run/maixcam-supervisor.lock 2>/dev/null || true
    exit 0
}
trap cleanup INT TERM EXIT
last_state=''
bad_media=0
while :; do
    if [ -e /boot/webrtc.disable ] || [ -e /root/webrtc.disable ]; then
        next=DISABLED
    elif ! "$NETWORK" status; then
        next=AP_STARTING
        "$NETWORK" start >> "$LOG" 2>&1 || next=AP_FAILED
    elif pidof launcher >/dev/null 2>&1; then
        next=MEDIA_OWNER_CONFLICT
    elif ! pidof stream_webrtc-m2 >/dev/null 2>&1; then
        next=MEDIA_STARTING
        /root/webrtc_service.sh start >> "$LOG" 2>&1 || next=MEDIA_FAILED
    elif ! netstat -tln | awk '$4 ~ /:800[01]$/ && $6 == "LISTEN" {n++} END {exit n < 2}'; then
        next=MEDIA_UNHEALTHY
        bad_media=$((bad_media + 1))
        if [ "$bad_media" -ge 3 ]; then
            /root/webrtc_service.sh restart >> "$LOG" 2>&1 || true
            bad_media=0
        fi
    else
        next=READY
        bad_media=0
    fi
    if [ "$next" != "$last_state" ]; then
        echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') $next" >> "$LOG"
        echo "STATE=$next" > "$STATE"
        last_state=$next
    fi
    # Bounded resource/log monitoring; no Wi-Fi or Bluetooth scans.
    for file in "$LOG" /root/maixcam-tests/standalone-web/hostapd.log; do
        if [ -f "$file" ] && [ "$(wc -c < "$file")" -gt 131072 ]; then
            cp "$file" "$file.previous"
            : > "$file"
        fi
    done
    sleep 10 &
    wait $! || true
done
