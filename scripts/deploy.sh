#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/stream_webrtc-m2"
DL_LIB="$ROOT/build/dl_lib"
REMOTE="${1:-root@192.168.1.8}"

echo "=== MAIXCAM WEBRTC DEPLOYMENT ==="
echo "Target: $REMOTE"

if [ ! -f "$BIN" ]; then
    echo "ERROR: Binary not found at $BIN. Run 'python3 scripts/build.py' first." >&2
    exit 1
fi

ACTUAL_SHA="$(sha256sum "$BIN" | awk '{print $1}')"
SHORT_SHA="${ACTUAL_SHA:0:8}"
STAGE="/root/g4c4-m2-stage-20261031-$SHORT_SHA"

echo "Binary SHA256: $ACTUAL_SHA"
echo "Stage Directory: $STAGE"

echo "1. Stopping active service on target..."
timeout -k 2s 20s ssh -T \
    -o ConnectTimeout=5 \
    -o StrictHostKeyChecking=no \
    "$REMOTE" 'sh -s' <<'REMOTE_STOP'
set -eu
if [ -f /root/webrtc_service.sh ]; then
    /root/webrtc_service.sh stop 2>/dev/null || true
fi
killall -INT stream_webrtc-m2 2>/dev/null || true
sleep 1
if pgrep stream_webrtc-m2 >/dev/null 2>&1; then
    killall -TERM stream_webrtc-m2 2>/dev/null || true
fi
REMOTE_STOP

echo "2. Preparing remote staging directory..."
timeout -k 2s 30s ssh -T \
    -o ConnectTimeout=5 \
    -o StrictHostKeyChecking=no \
    "$REMOTE" "sh -s" <<REMOTE_PREP
set -eu
STAGE="$STAGE"
if [ ! -d "\$STAGE" ]; then
    (umask 077; mkdir -p "\$STAGE/dl_lib")
fi
REMOTE_PREP

echo "3. Synchronizing dynamic libraries (dl_lib)..."
if [ -d "$DL_LIB" ]; then
    scp -rq "$DL_LIB"/* "$REMOTE:$STAGE/dl_lib/"
fi

echo "4. Transferring binary executable..."
cat "$BIN" | timeout -k 2s 60s ssh -T \
    -o ConnectTimeout=5 \
    -o StrictHostKeyChecking=no \
    "$REMOTE" "umask 077; cat > '$STAGE/stream_webrtc-m2'"

echo "5. Verifying and launching service..."
timeout -k 2s 30s ssh -T \
    -o ConnectTimeout=5 \
    -o StrictHostKeyChecking=no \
    "$REMOTE" "sh -s" <<REMOTE_LAUNCH
set -eu
STAGE="$STAGE"
BIN="\$STAGE/stream_webrtc-m2"
EXPECTED_SHA="$ACTUAL_SHA"

chmod 700 "\$BIN"
ACTUAL="\$(sha256sum "\$BIN" | awk '{print \$1}')"
if [ "\$ACTUAL" != "\$EXPECTED_SHA" ]; then
    echo "ERROR: Binary SHA mismatch on target!"
    exit 2
fi

if [ -f /root/webrtc_service.sh ]; then
    sed -i "s|^STAGE=.*|STAGE=\"\$STAGE\"|" /root/webrtc_service.sh
    /root/webrtc_service.sh start
else
    cd "\$STAGE"
    export LD_LIBRARY_PATH="\$STAGE/dl_lib:/usr/lib:/lib"
    export MAIX_WEBRTC_AUDIO_SOURCE="mic"
    export MAIX_WEBRTC_BT_DEVICE="bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=a2dp"
    nohup "\$BIN" > "\$STAGE/m2_runtime.log" 2>&1 &
    echo \$! > "\$STAGE/m2.pid"
fi

sleep 2
PID=\$(cat "\$STAGE/m2.pid" 2>/dev/null || pgrep stream_webrtc-m2 || true)
if [ -n "\$PID" ] && kill -0 "\$PID" 2>/dev/null; then
    echo "SERVICE_RUNNING=PASS (PID \$PID)"
else
    echo "SERVICE_RUNNING=FAIL"
    tail -n 25 "\$STAGE/m2_runtime.log" 2>/dev/null || true
    exit 1
fi
REMOTE_LAUNCH

echo "DEPLOYMENT=SUCCESS"
