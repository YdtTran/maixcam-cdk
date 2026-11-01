#!/bin/sh
# Only wlan0 is owned here. USB, SSH, Bluetooth and other interfaces are untouched.
set -eu
CONFIG=/etc/maixcam-standalone
STATE=/root/maixcam-tests/standalone-web
alive() { [ -s "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }
stop_pid() {
    if alive "$1"; then kill "$(cat "$1")" 2>/dev/null || true; fi
    rm -f "$1"
}
ready() {
    alive /run/maixcam-hostapd.pid && alive /run/maixcam-dnsmasq.pid &&
    ip -4 addr show dev wlan0 | grep -q '192.168.4.1/24' &&
    hostapd_cli -p /run/maixcam-hostapd -i wlan0 status | grep -q '^state=ENABLED'
}
stop() {
    stop_pid /run/maixcam-dnsmasq.pid
    stop_pid /run/maixcam-hostapd.pid
    ip addr del 192.168.4.1/24 dev wlan0 2>/dev/null || true
}
start() {
    ready && return 0
    [ -e "$CONFIG/enabled" ] || return 1
    [ -e /sys/class/net/wlan0 ] || return 1
    mkdir -p "$STATE"
    dnsmasq --test --conf-file="$CONFIG/dnsmasq.conf"
    stop
    # The original STA launcher is bypassed at boot. Stop only wlan0 clients now.
    wpa_cli -i wlan0 terminate >/dev/null 2>&1 || true
    if [ -s /run/udhcpc.wlan0.pid ]; then
        stop_pid /run/udhcpc.wlan0.pid
    fi
    ip link set wlan0 up
    ip -4 addr flush dev wlan0
    ip addr add 192.168.4.1/24 dev wlan0
    # No default route or NAT is required for direct local communication.
    start_radio() {
        hostapd -B -P /run/maixcam-hostapd.pid -f "$STATE/hostapd.log" "$1" || return 1
        n=0
        until hostapd_cli -p /run/maixcam-hostapd -i wlan0 status | grep -q '^state=ENABLED'; do
            n=$((n + 1))
            [ "$n" -lt 15 ] || return 1
            sleep 1
        done
        echo "$1" > /run/maixcam-ap-config
    }
    if ! start_radio "$CONFIG/hostapd.conf"; then
        stop_pid /run/maixcam-hostapd.pid
        if [ ! -f "$CONFIG/hostapd-fallback.conf" ] || ! start_radio "$CONFIG/hostapd-fallback.conf"; then
            stop
            return 1
        fi
    fi
    dnsmasq --conf-file="$CONFIG/dnsmasq.conf" || { stop; return 1; }
    ready
}
case "${1:-status}" in
    start) start ;;
    stop) stop ;;
    restart) stop; start ;;
    status) ready ;;
    *) exit 2 ;;
esac
