#!/bin/sh
STAGE="/root/g4c4-m2-stage-20261031-dbc06023"
BIN="$STAGE/stream_webrtc-m2"
LOG="$STAGE/m2_runtime.log"
PIDFILE="$STAGE/m2.pid"

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

    # Ensure bluetooth & bluealsa are ready
    if ! ps | grep -v grep | grep -q bluealsa >/dev/null 2>&1; then
        /etc/init.d/S41bluealsa start
        sleep 1
    fi

    cd "$STAGE"
    export LD_LIBRARY_PATH="$STAGE/dl_lib:/usr/lib:/lib"
    export MAIX_WEBRTC_AUDIO_SOURCE="mic"
    export MAIX_WEBRTC_BT_DEVICE="bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=a2dp"

    echo "Starting WebRTC M2 candidate..."
    nohup "$BIN" > "$LOG" 2>&1 &
    PID=$!
    echo "$PID" > "$PIDFILE"
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
