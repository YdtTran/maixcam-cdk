#!/bin/sh
STAGE="/root/g4c4-m2-stage-20261031-dbc06023"
BIN="$STAGE/stream_webrtc-m2"
LOG="$STAGE/m2_runtime.log"
PIDFILE="$STAGE/m2.pid"
BT_PIDFILE="$STAGE/bt_profile_watch.pid"

prepare_bluetooth_controller() {
    target_mac="6C:16:29:7B:00:D4"
    hciconfig hci0 up >/dev/null 2>&1 || true
    hciconfig hci0 voice 0x0060 >/dev/null 2>&1 || true
    if ! hciconfig -a hci0 2>/dev/null | grep -q "SCO MTU: 120:4"; then
        if hcitool -i hci0 con 2>/dev/null | grep -q "eSCO"; then
            bluetoothctl disconnect "$target_mac" >/dev/null 2>&1 || true
            sleep 2
        fi
        hciconfig hci0 scomtu 120:4 >/dev/null 2>&1 || true
    fi
}

connect_bluetooth_mic() {
    target_mac="6C:16:29:7B:00:D4"
    # Fast check: if BlueALSA already exposes the SCO PCM, do not mutate the link or controller.
    if bluealsa-aplay -L 2>/dev/null | grep -q "DEV=${target_mac},PROFILE=sco"; then
        return 0
    fi

    prepare_bluetooth_controller

    if ! bluetoothctl info "$target_mac" 2>/dev/null | grep -q "Connected: yes"; then
        bluetoothctl connect "$target_mac" >/dev/null 2>&1 || true
        sleep 2
    fi

    if ! bluealsa-aplay -L 2>/dev/null | grep -q "DEV=${target_mac},PROFILE=sco"; then
        dbus-send --system --print-reply --reply-timeout=3000 --dest=org.bluez \
            /org/bluez/hci0/dev_6C_16_29_7B_00_D4 \
            org.bluez.Device1.ConnectProfile \
            string:0000111e-0000-1000-8000-00805f9b34fb >/dev/null 2>&1 || true
        sleep 1
    fi

    if bluealsa-aplay -L 2>/dev/null | grep -q "DEV=${target_mac},PROFILE=sco"; then
        amixer -D bluealsa sset "EDIFIER X2 Plus SCO" 100% unmute >/dev/null 2>&1 || true
    fi
}

stop_bluetooth_watch() {
    if [ -f "$BT_PIDFILE" ]; then
        kill "$(cat "$BT_PIDFILE")" 2>/dev/null || true
        rm -f "$BT_PIDFILE"
    fi
}

watch_bluetooth() {
    media_pid=$1
    while kill -0 "$media_pid" 2>/dev/null; do
        connect_bluetooth_mic
        sleep 5
    done
}

start() {
    if [ -f "$PIDFILE" ] && kill -0 $(cat "$PIDFILE") 2>/dev/null; then
        echo "WebRTC M2 is already running (PID $(cat $PIDFILE))"
        return 0
    fi

    # Check competing processes
    if ps | grep -E "stream_webrtc" | grep -v grep | grep -v "webrtc_service" >/dev/null; then
        echo "Error: another stream_webrtc process is running!"
        return 1
    fi

    restart_bluealsa=0
    shim="$STAGE/dl_lib/libg4c4_sco_write_align.so"
    # BlueALSA 4.1.1 consumes 254 PCM bytes but writes 255 for an odd SCO
    # MTU. Apply an outgoing-only compatibility shim; leave HCI settings alone.
    if [ -f "$shim" ] && bluealsa --version | grep -q '4\.1\.1'; then
        if ! grep -Fq "LD_PRELOAD=$shim " /etc/init.d/S41bluealsa; then
            cp -p /etc/init.d/S41bluealsa /etc/init.d/S41bluealsa.before-sco-align
            sed -i "s|LD_PRELOAD=[^ ]* ||; s|start-stop-daemon -S|LD_PRELOAD=$shim start-stop-daemon -S|" /etc/init.d/S41bluealsa
            restart_bluealsa=1
        fi
    fi

    # Earbud microphone requires the hands-free audio gateway profile.
    if ! grep -q -- '-p hfp-ag' /etc/init.d/S41bluealsa; then
        cp -p /etc/init.d/S41bluealsa /etc/init.d/S41bluealsa.before-earbud-mic
        sed -i 's/-p a2dp-source/-p a2dp-source -p hfp-ag/' /etc/init.d/S41bluealsa
        restart_bluealsa=1
    fi
    if [ "$restart_bluealsa" = 1 ]; then
        /etc/init.d/S41bluealsa restart
        sleep 2
    fi

    # Ensure S42bt_manager does not run duplicate connection loops alongside bt-watch
    if [ -x /etc/init.d/S42bt_manager ]; then
        /etc/init.d/S42bt_manager stop >/dev/null 2>&1 || true
    fi

    # Ensure bluetooth & bluealsa are ready
    if ! ps | grep -v grep | grep -q bluealsa >/dev/null 2>&1; then
        /etc/init.d/S41bluealsa start
        sleep 1
    fi

    prepare_bluetooth_controller
    connect_bluetooth_mic

    cd "$STAGE"
    export LD_LIBRARY_PATH="$STAGE/dl_lib:/usr/lib:/lib"
    export MAIX_WEBRTC_AUDIO_SOURCE="mic"
    export MAIX_WEBRTC_BT_DEVICE="bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=sco"

    echo "Starting WebRTC M2 candidate..."
    nohup "$BIN" > "$LOG" 2>&1 &
    PID=$!
    echo "$PID" > "$PIDFILE"
    stop_bluetooth_watch
    nohup "$0" bt-watch "$PID" </dev/null >/dev/null 2>&1 &
    echo $! > "$BT_PIDFILE"
    sleep 2

    if kill -0 "$PID" 2>/dev/null; then
        echo "WebRTC M2 started successfully (PID $PID)"
        return 0
    else
        echo "Failed to start WebRTC M2. Check $LOG"
        return 1
    fi
}

stop() {
    stop_bluetooth_watch
    if [ -f "$PIDFILE" ]; then
        PID=$(cat "$PIDFILE")
        if kill -0 "$PID" 2>/dev/null; then
            echo "Stopping WebRTC M2 (PID $PID)..."
            kill -INT "$PID" 2>/dev/null
            for i in $(seq 1 10); do
                if ! kill -0 "$PID" 2>/dev/null; then
                    break
                fi
                sleep 0.5
            done
            if kill -0 "$PID" 2>/dev/null; then
                kill -TERM "$PID" 2>/dev/null
            fi
        fi
        rm -f "$PIDFILE"
    fi
    killall stream_webrtc-m2 2>/dev/null || true
    echo "WebRTC M2 stopped."
}

status() {
    if [ -f "$PIDFILE" ] && kill -0 $(cat "$PIDFILE") 2>/dev/null; then
        echo "WebRTC M2 is RUNNING (PID $(cat $PIDFILE))"
        echo "Ports:"
        netstat -tln | grep -E "8000|8001"
        echo "Recent Log:"
        tail -n 15 "$LOG"
    else
        echo "WebRTC M2 is STOPPED"
    fi
}

case "$1" in
    bt-watch)
        case "${2:-}" in ''|*[!0-9]*) exit 2 ;; esac
        watch_bluetooth "$2"
        ;;
    start|stop|status)
        "$1"
        ;;
    restart)
        stop
        sleep 1
        start
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac
