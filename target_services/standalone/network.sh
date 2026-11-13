#!/bin/sh
# wlan0 owns Setup AP (192.168.4.1/24); wlan1 owns STA Wi-Fi.
# USB, SSH, Bluetooth and other interfaces are untouched.
set -eu
CONFIG=/etc/maixcam-standalone
STATE=/root/maixcam-tests/standalone-web
WPA_CONF=/etc/wpa_supplicant.conf
FORCE_AP=/run/maixcam-force-ap
PROV_STATE=/run/maixcam-provision.state

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

ensure_wlan1() {
    [ -e /sys/class/net/wlan1 ] || iw phy phy0 interface add wlan1 type managed
}

ready_sta() {
    [ -e /sys/class/net/wlan1 ] &&
    ip -4 addr show dev wlan1 2>/dev/null | grep -q 'inet ' &&
    iw dev wlan1 link 2>/dev/null | grep -q 'Connected to'
}

ready_ap() {
    alive /run/maixcam-hostapd.pid && alive /run/maixcam-dnsmasq.pid &&
    ip -4 addr show dev wlan0 2>/dev/null | grep -q '192.168.4.1/24' &&
    hostapd_cli -p /run/maixcam-hostapd -i wlan0 status 2>/dev/null | grep -q '^state=ENABLED'
}

ready() {
    ready_sta || ready_ap
}

stop_ap() {
    stop_pid /run/maixcam-dnsmasq.pid
    stop_pid /run/maixcam-hostapd.pid
    killall hostapd dnsmasq >/dev/null 2>&1 || true
    rm -rf /run/maixcam-hostapd /run/maixcam-ap-config
    ip addr del 192.168.4.1/24 dev wlan0 2>/dev/null || true
    ip -4 addr flush dev wlan0 2>/dev/null || true
    ip link set wlan0 down 2>/dev/null || true
}

stop_sta() {
    wpa_cli -i wlan1 terminate >/dev/null 2>&1 || true
    wpa_cli -i wlan0 terminate >/dev/null 2>&1 || true
    killall wpa_supplicant >/dev/null 2>&1 || true
    stop_pid /run/udhcpc.wlan1.pid
    stop_pid /run/udhcpc.wlan0.pid
    [ -e /sys/class/net/wlan1 ] && ip -4 addr flush dev wlan1 2>/dev/null || true
}

stop() {
    stop_ap
    stop_sta
}

start_sta() {
    ensure_wlan1 || return 1
    ip link set wlan1 up
    ip -4 addr flush dev wlan1 2>/dev/null || true
    wpa_supplicant -B -i wlan1 -c "$WPA_CONF" >/dev/null 2>&1 || return 1
    n=0
    until iw dev wlan1 link 2>/dev/null | grep -q 'Connected to'; do
        n=$((n + 1))
        [ "$n" -lt 12 ] || return 1
        sleep 1
    done
    udhcpc -i wlan1 -p /run/udhcpc.wlan1.pid -t 6 -T 2 -n >/dev/null 2>&1 || return 1
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
            stop_ap
            return 1
        fi
    fi
    dnsmasq --conf-file="$CONFIG/dnsmasq.conf" || { stop_ap; return 1; }
    ready_ap
}

provision_sta() {
    rm -f "$FORCE_AP"
    echo "connecting" > "$PROV_STATE"
    stop_sta
    if start_sta; then
        ip=$(ip -4 -o addr show dev wlan1 | awk '{print $4}' | cut -d/ -f1 | head -n1)
        echo "ok:$ip" > "$PROV_STATE"
        # Keep Setup AP alive briefly so the browser displays the new IP, then close AP.
        sleep 12
        stop_ap
        echo "idle" > "$PROV_STATE"
        return 0
    fi
    stop_sta
    echo "fail" > "$PROV_STATE"
    return 1
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
        stop_sta
    fi
    start_ap
}

case "${1:-status}" in
    start) start ;;
    stop) stop ;;
    stop_ap) stop_ap ;;
    provision_sta) provision_sta ;;
    restart) rm -f "$FORCE_AP"; stop; start ;;
    ap) touch "$FORCE_AP"; stop; start_ap ;;
    status) ready ;;
    *) exit 2 ;;
esac
