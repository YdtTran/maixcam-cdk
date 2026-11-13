#!/bin/sh
# Only wlan0 is owned here. USB, SSH, Bluetooth and other interfaces are untouched.
# Normal operation connects wlan0 in STA mode via /etc/wpa_supplicant.conf.
# Access point (192.168.4.1/24) is used as a fallback for Wi-Fi setup only.
set -eu
CONFIG=/etc/maixcam-standalone
STATE=/root/maixcam-tests/standalone-web
WPA_CONF=/etc/wpa_supplicant.conf
FORCE_AP=/run/maixcam-force-ap

alive() { [ -s "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }
stop_pid() {
    if alive "$1"; then kill "$(cat "$1")" 2>/dev/null || true; fi
    rm -f "$1"
}

has_sta_config() {
    [ -s "$WPA_CONF" ] && awk '
        /^[[:space:]]*network[[:space:]]*=\{/ { in_blk=1; dis=0; next }
        in_blk && /^[[:space:]]*disabled[[:space:]]*=[[:space:]]*1/ { dis=1 }
        in_blk && /^[[:space:]]*\}/ { if (!dis) ok=1; in_blk=0 }
        END { exit !ok }
    ' "$WPA_CONF"
}

ready_sta() {
    ! alive /run/maixcam-hostapd.pid &&
    ip -4 addr show dev wlan0 2>/dev/null | grep -q 'inet ' &&
    ! ip -4 addr show dev wlan0 2>/dev/null | grep -q '192.168.4.1/24' &&
    iw dev wlan0 link 2>/dev/null | grep -q 'Connected to'
}

ready_ap() {
    alive /run/maixcam-hostapd.pid && alive /run/maixcam-dnsmasq.pid &&
    ip -4 addr show dev wlan0 2>/dev/null | grep -q '192.168.4.1/24' &&
    hostapd_cli -p /run/maixcam-hostapd -i wlan0 status 2>/dev/null | grep -q '^state=ENABLED'
}

ready() {
    ready_sta || ready_ap
}

stop() {
    stop_pid /run/maixcam-dnsmasq.pid
    stop_pid /run/maixcam-hostapd.pid
    wpa_cli -i wlan0 terminate >/dev/null 2>&1 || true
    killall wpa_supplicant >/dev/null 2>&1 || true
    stop_pid /run/udhcpc.wlan0.pid
    ip -4 addr flush dev wlan0 2>/dev/null || true
}

start_sta() {
    ip link set wlan0 up
    ip -4 addr flush dev wlan0
    wpa_supplicant -B -i wlan0 -c "$WPA_CONF" >/dev/null 2>&1 || return 1
    n=0
    until iw dev wlan0 link 2>/dev/null | grep -q 'Connected to'; do
        n=$((n + 1))
        [ "$n" -lt 25 ] || return 1
        sleep 1
    done
    udhcpc -i wlan0 -p /run/udhcpc.wlan0.pid -t 6 -T 2 -n >/dev/null 2>&1 || return 1
    ready_sta
}

start_ap() {
    dnsmasq --test --conf-file="$CONFIG/dnsmasq.conf"
    ip link set wlan0 up
    ip -4 addr flush dev wlan0
    ip addr add 192.168.4.1/24 dev wlan0
    start_radio() {
        hostapd -B -P /run/maixcam-hostapd.pid -f "$STATE/hostapd.log" "$1" || return 1
        n=0
        until hostapd_cli -p /run/maixcam-hostapd -i wlan0 status 2>/dev/null | grep -q '^state=ENABLED'; do
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
    ready_ap
}

start() {
    ready && return 0
    [ -e "$CONFIG/enabled" ] || return 1
    [ -e /sys/class/net/wlan0 ] || return 1
    mkdir -p "$STATE"
    stop
    if [ ! -e "$FORCE_AP" ] && has_sta_config; then
        if start_sta; then
            return 0
        fi
        stop
    fi
    start_ap
}

case "${1:-status}" in
    start) start ;;
    stop) stop ;;
    restart) rm -f "$FORCE_AP"; stop; start ;;
    ap) touch "$FORCE_AP"; stop; start_ap ;;
    status) ready ;;
    *) exit 2 ;;
esac
