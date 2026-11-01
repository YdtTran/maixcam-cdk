#!/bin/sh
# Runs on MaixCAM from a unique staged release, reached through verified USB SSH.
set -eu
STAGE=$1
CONFIG=/etc/maixcam-standalone
EVIDENCE=/root/maixcam-tests/standalone-web
BACKUP="$STAGE/backup"
mkdir -p "$BACKUP" "$EVIDENCE"
chmod 700 "$STAGE" "$BACKUP"
# Every managed path, including absent paths, is recorded for exact rollback.
cat > "$BACKUP/paths" <<'EOF'
etc/init.d/S30wifi
etc/init.d/S30wifi.pre-standalone
etc/init.d/S99webrtc_autostart
etc/maixcam-standalone
root/webrtc_service.sh
root/webrtc_autostart_daemon.sh
root/maixcam_standalone_network.sh
EOF
: > "$BACKUP/present"
: > "$BACKUP/absent"
while IFS= read -r path; do
    if [ -e "/$path" ]; then echo "$path" >> "$BACKUP/present"; else echo "$path" >> "$BACKUP/absent"; fi
done < "$BACKUP/paths"
tar -cf "$BACKUP/config.tar" -C / -T "$BACKUP/present"
# Read-only snapshot of network/boot inputs; credentials remain root-only.
tar -cf "$BACKUP/network-original.tar" /etc/network/interfaces /etc/wpa_supplicant.conf /boot/wifi.sta /boot/wifi.ssid /boot/wifi.pass /etc/rc.local 2>/dev/null
cat > "$STAGE/rollback.sh" <<EOF
#!/bin/sh
set -eu
touch '$STAGE/rollback-completed'
# Use USB: ssh root@10.172.16.1 'sh $STAGE/rollback.sh'
if [ -s /run/webrtc_autostart.pid ]; then
    kill "\$(cat /run/webrtc_autostart.pid)" 2>/dev/null || true
    sleep 2
fi
/root/webrtc_service.sh stop || true
/root/maixcam_standalone_network.sh stop || true
rm -f /run/webrtc_autostart.pid
rmdir /run/maixcam-supervisor.lock 2>/dev/null || true
while IFS= read -r path; do rm -rf "/\$path"; done < '$BACKUP/absent'
tar -xf '$BACKUP/config.tar' -C /
# Factory STA credentials and USB services were never changed.
/etc/init.d/S30wifi start
/etc/init.d/S99webrtc_autostart start
EOF
chmod 700 "$STAGE/rollback.sh"
# Arm rollback before stopping anything. Survives lost deployment SSH.
cat > "$STAGE/rollback-watch.sh" <<EOF
#!/bin/sh
sleep 180
[ -e '$STAGE/health-committed' ] || [ -e '$STAGE/rollback-completed' ] || exec sh '$STAGE/rollback.sh'
EOF
chmod 700 "$STAGE/rollback-watch.sh"
nohup sh "$STAGE/rollback-watch.sh" > "$EVIDENCE/rollback-watch.log" 2>&1 &
echo $! > "$STAGE/rollback-watch.pid"
# Stop old supervisor before changing its executable (single launcher/owner).
/etc/init.d/S99webrtc_autostart stop
# Stop only the known wlan0 STA processes, never USB networking or SSH.
wpa_cli -i wlan0 terminate >/dev/null 2>&1 || true
if [ -s /run/udhcpc.wlan0.pid ]; then
    kill "$(cat /run/udhcpc.wlan0.pid)" 2>/dev/null || true
    rm -f /run/udhcpc.wlan0.pid
fi
if [ ! -e /etc/init.d/S30wifi.pre-standalone ]; then
    cp -p /etc/init.d/S30wifi /etc/init.d/S30wifi.pre-standalone
fi
mkdir -p "$CONFIG"
chmod 700 "$CONFIG"
for file in hostapd.conf hostapd-fallback.conf dnsmasq.conf server.crt server.key ca.crt signal-token; do
    cp "$STAGE/config/$file" "$CONFIG/$file"
    chmod 600 "$CONFIG/$file"
done
cp "$STAGE/services/S30wifi" /etc/init.d/S30wifi
cp "$STAGE/services/S99webrtc_autostart" /etc/init.d/S99webrtc_autostart
cp "$STAGE/services/network.sh" /root/maixcam_standalone_network.sh
cp "$STAGE/services/supervisor.sh" /root/webrtc_autostart_daemon.sh
# Preserve the known-good service's media environment, gains and BT parameters.
sed "/^export MAIX_WEBRTC_STANDALONE=/d; /^export MAIX_WEBRTC_TLS_CERT=/d; /^export MAIX_WEBRTC_TLS_KEY=/d; /^export MAIX_WEBRTC_SIGNAL_TOKEN=/d; s|^STAGE=.*|STAGE=\"$STAGE\"|" "$BACKUP/../webrtc_service.original" > /root/webrtc_service.sh
sed -i '/^BIN=/i\export MAIX_WEBRTC_STANDALONE=1\nexport MAIX_WEBRTC_TLS_CERT=/etc/maixcam-standalone/server.crt\nexport MAIX_WEBRTC_TLS_KEY=/etc/maixcam-standalone/server.key\nexport MAIX_WEBRTC_SIGNAL_TOKEN="$(cat /etc/maixcam-standalone/signal-token)"' /root/webrtc_service.sh
chmod 700 /root/webrtc_service.sh /root/webrtc_autostart_daemon.sh /root/maixcam_standalone_network.sh
chmod 755 /etc/init.d/S30wifi /etc/init.d/S99webrtc_autostart
chmod 700 "$STAGE/stream_webrtc-m2"
# enabled is the final boot switch; original STA inputs remain intact.
touch "$CONFIG/enabled"
/etc/init.d/S99webrtc_autostart start
echo "ROLLBACK=sh $STAGE/rollback.sh"
