#!/usr/bin/env bash
# Unprivileged, hardware-free checks. Never connect to the host system bus.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
package=$(readlink -f "${1:-$root/result/floss}")
report=$(realpath -m "${2:-$root/runtime-results/floss}")
mkdir -p "$report"
bwrap_bin=$(command -v bwrap)
dbus_bin=$(readlink -f "$(command -v dbus-daemon)")
busctl_bin=$(readlink -f "$(command -v busctl)")
timeout_bin="$(dirname "$(readlink -f "$(command -v timeout)")")/timeout"
smoke_path="$(dirname "$(readlink -f "$(command -v mkdir)")"):$(dirname "$(readlink -f "$(command -v grep)")"):/bin"
cat > "$report/bus.conf" <<'CONF'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=/tmp/floss-private-bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow own="*"/>
    <allow send_destination="*"/>
    <allow receive_sender="*"/>
  </policy>
</busconfig>
CONF
cat > "$report/inside.sh" <<'INNER'
#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
export PATH="$FLOSS_PACKAGE/bin:$SMOKE_PATH"
export HOME=/tmp/home XDG_RUNTIME_DIR=/tmp/runtime
mkdir -p "$HOME" "$XDG_RUNTIME_DIR" /var/lib/bluetooth /var/log/bluetooth /run/bluetooth
ln -s /run /var/run
chmod 700 "$XDG_RUNTIME_DIR"
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/floss-private-bus
export DBUS_SESSION_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS"
help_ok=true
for name in btmanagerd btadapterd btclient; do
  if "$FLOSS_PACKAGE/bin/$name" --help > "/report/$name-help.txt" 2>&1; then
    printf '%s_help=0\n' "$name"
  else
    printf '%s_help=%s\n' "$name" "$?"
    help_ok=false
  fi
done
bus_pid= manager_pid=
cleanup() {
  if [ -n "$manager_pid" ]; then kill "$manager_pid" 2>/dev/null || true; wait "$manager_pid" 2>/dev/null || true; fi
  if [ -n "$bus_pid" ]; then kill "$bus_pid" 2>/dev/null || true; wait "$bus_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT
"$DBUS_DAEMON" --nofork --config-file=/report/bus.conf > /report/private-bus.log 2>&1 &
bus_pid=$!
for i in {1..50}; do [ -S /tmp/floss-private-bus ] && break; sleep 0.1; done
[ -S /tmp/floss-private-bus ]
"$FLOSS_PACKAGE/bin/btmanagerd" --systemd --log-output=stderr > /report/manager.log 2>&1 &
manager_pid=$!
ready=false
for i in {1..50}; do
  if "$BUSCTL" --address="$DBUS_SYSTEM_BUS_ADDRESS" --timeout=1 call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus NameHasOwner s org.chromium.bluetooth.Manager 2>/dev/null | grep -q 'true'; then ready=true; break; fi
  sleep 0.1
done
printf 'manager_name_owned=%s\n' "$ready"
if "$ready"; then
  "$BUSCTL" --address="$DBUS_SYSTEM_BUS_ADDRESS" --timeout=2 introspect org.chromium.bluetooth.Manager /org/chromium/bluetooth/Manager > /report/manager-introspection.txt
  for method in GetAvailableAdapters GetDefaultAdapter GetFlossEnabled GetFlossApiVersion; do
    printf '%s: ' "$method"
    "$BUSCTL" --address="$DBUS_SYSTEM_BUS_ADDRESS" --timeout=2 call org.chromium.bluetooth.Manager /org/chromium/bluetooth/Manager org.chromium.bluetooth.Manager "$method"
  done
  # The namespace has no sysfs radios or /dev/rfkill. Check the real new API
  # signature and fail-closed behavior without any possible host radio write.
  for adapter in 0 -1 65534; do
    rfkill_state=$("$BUSCTL" --address="$DBUS_SYSTEM_BUS_ADDRESS" --timeout=2 -- call org.chromium.bluetooth.Manager /org/chromium/bluetooth/Manager org.chromium.bluetooth.Manager GetRfkillState i "$adapter")
    printf 'GetRfkillState(%s): %s\n' "$adapter" "$rfkill_state"
    printf '%s\n' "$rfkill_state" | grep -Fq '"available" b false'
    rfkill_set=$("$BUSCTL" --address="$DBUS_SYSTEM_BUS_ADDRESS" --timeout=2 -- call org.chromium.bluetooth.Manager /org/chromium/bluetooth/Manager org.chromium.bluetooth.Manager SetRfkillBlocked ib "$adapter" false)
    [ "$rfkill_set" = 'b false' ]
  done
fi
kill "$manager_pid" 2>/dev/null || true
wait "$manager_pid" 2>/dev/null || true
manager_pid=
set +e
"$TIMEOUT" -k 2 8 "$FLOSS_PACKAGE/bin/btadapterd" --hci=65534 --index=65534 --log-output=stderr > /report/adapter-no-hardware.log 2>&1
adapter_status=$?
set -e
printf 'adapter_no_hardware_exit=%s\n' "$adapter_status"
# In this namespace AF_BLUETOOTH is absent. The HAL requests SIGTERM and the
# adapter must finish its shutdown path cleanly, without a signal or timeout.
"$ready" && "$help_ok" && [ "$adapter_status" -eq 0 ]
INNER
# Empty network, PID, IPC, UTS namespaces and no capabilities. /sys is empty;
# /dev is synthetic, and /run and /var are private. Host D-Bus sockets are absent.
exec "$bwrap_bin" --die-with-parent --unshare-all --cap-drop ALL \
  --tmpfs / --ro-bind /nix/store /nix/store --ro-bind /etc /etc \
  --ro-bind /bin /bin --ro-bind /usr /usr --proc /proc --dev /dev \
  --tmpfs /tmp --tmpfs /run --tmpfs /var --dir /sys \
  --bind "$report" /report --setenv FLOSS_PACKAGE "$package" \
  --setenv DBUS_DAEMON "$dbus_bin" --setenv BUSCTL "$busctl_bin" \
  --setenv TIMEOUT "$timeout_bin" --setenv SMOKE_PATH "$smoke_path" /bin/sh /report/inside.sh
