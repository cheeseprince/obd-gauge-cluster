#!/usr/bin/env bash
# hilctl.sh — start, stop and health-check the HIL emulator pair.
#
# The rig is two cooperating processes:
#
#   elm_server.py   the ELM327 protocol emulator, listening on TCP 35000
#   ble_elm.py      a BlueZ GATT server that advertises as an OBD adapter and
#                   bridges the dash's BLE writes/notifies to that TCP socket
#
# The README used to say "run them both with &". That works until you need to
# stop them, at which point you have two orphans, no PIDs, and a port still
# held. This script owns their lifecycle instead.
#
# ⚠️ IT NEVER PATTERN-KILLS. `pkill -f ble_elm.py` matches the shell running the
# pkill as well, so it kills the caller — that has actually happened here and
# cost a debugging session. Everything below stops processes by PID from a
# pidfile, and verifies the PID is still the process it thinks it is.
#
#   ./hilctl.sh start | stop | restart | status | health | logs
#
# If the systemd user units are installed (see README), start/stop/restart
# delegate to systemctl so the two mechanisms cannot fight over the same
# processes.
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

SCENARIO="${HIL_SCENARIO:-gm_sierra}"
PORT="${HIL_PORT:-35000}"
PROFILE="${HIL_PROFILE:-vlinker}"
ADV_NAME="${HIL_NAME:-vLinker MS-B}"

RUN_DIR="${XDG_RUNTIME_DIR:-/tmp}"
ELM_PID="$RUN_DIR/hil_elm.pid"
SHIM_PID="$RUN_DIR/hil_shim.pid"
# Overridable so a test run cannot truncate the logs of a rig that is actually
# running -- which is exactly what happened the first time this was exercised.
ELM_LOG="${HIL_ELM_LOG:-/tmp/hil_elm.log}"
SHIM_LOG="${HIL_SHIM_LOG:-/tmp/hil_shim.log}"

have_systemd_units() {
  systemctl --user cat hil-emulator.service >/dev/null 2>&1
}

# True if $1 is a pidfile whose PID is alive AND whose command line still
# contains $2. A bare kill -0 would happily match a recycled PID.
alive() {
  local pidfile="$1" want="$2" pid
  [ -f "$pidfile" ] || return 1
  pid="$(cat "$pidfile" 2>/dev/null)" || return 1
  [ -n "$pid" ] && [ -d "/proc/$pid" ] || return 1
  tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q -- "$want"
}

wait_for_hci0() {
  # A cold boot enumerates the BLE dongle well after userspace is up. The shim
  # exits immediately if hci0 is missing, and the failure looks like a crash
  # rather than a race, so wait for it explicitly.
  for _ in $(seq 1 60); do
    hciconfig hci0 2>/dev/null | grep -q "UP RUNNING" && return 0
    sleep 1
  done
  echo "hilctl: hci0 never came UP RUNNING — is the BLE dongle attached?" >&2
  return 1
}

wait_for_port() {
  # The shim is a TCP client of the emulator. Starting it before the emulator
  # has bound loses the race and it exits.
  for _ in $(seq 1 30); do
    (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && { exec 3>&- 2>/dev/null; return 0; }
    sleep 1
  done
  echo "hilctl: emulator never opened port $PORT — see $ELM_LOG" >&2
  return 1
}

start() {
  if have_systemd_units; then
    echo "hilctl: systemd units present — delegating to systemctl"
    systemctl --user start hil-emulator.service && status
    return
  fi
  alive "$ELM_PID" elm_server.py && alive "$SHIM_PID" ble_elm.py && {
    echo "hilctl: already running"; status; return 0; }

  # PYTHONUNBUFFERED: without it Python block-buffers stdout when it is a file
  # rather than a tty, and the log stays empty for minutes — which reads
  # exactly like a dead emulator while you are debugging one.
  : > "$ELM_LOG"
  PYTHONUNBUFFERED=1 nohup python3 elm_server.py --scenario "$SCENARIO" --port "$PORT" \
      >> "$ELM_LOG" 2>&1 &
  echo $! > "$ELM_PID"
  echo "hilctl: emulator pid $(cat "$ELM_PID") scenario=$SCENARIO port=$PORT -> $ELM_LOG"

  wait_for_port || { stop; return 1; }
  wait_for_hci0 || { stop; return 1; }

  : > "$SHIM_LOG"
  PYTHONUNBUFFERED=1 nohup python3 ble_elm.py --profile "$PROFILE" --name "$ADV_NAME" \
      --host 127.0.0.1 --port "$PORT" >> "$SHIM_LOG" 2>&1 &
  echo $! > "$SHIM_PID"
  # Confirm it SURVIVED. The shim exits within a second if something else
  # already owns the adapter (a second copy, or a stale systemd unit), and
  # without this check the script would report success over a dead rig -- the
  # same misleading-status failure the old systemd unit had.
  sleep 2
  if ! alive "$SHIM_PID" ble_elm.py; then
    echo "hilctl: shim exited immediately — last lines of $SHIM_LOG:" >&2
    tail -n 5 "$SHIM_LOG" >&2
    stop
    return 1
  fi
  echo "hilctl: shim     pid $(cat "$SHIM_PID") profile=$PROFILE name='$ADV_NAME' -> $SHIM_LOG"

  echo
  echo "⚠️  The dash keeps its old link across a shim restart and goes quiet —"
  echo "    LE still connected, zero GATT traffic. Hard-reset the board:"
  echo "      python3 -c \"import serial,time; s=serial.Serial('/dev/ttyUSB0',115200);"
  echo "                   s.setDTR(False); s.setRTS(True); time.sleep(0.2); s.setRTS(False); s.close()\""
  echo "    Merely opening the port does NOT reset it if stty -hupcl was set."
}

stop_one() {
  local pidfile="$1" want="$2" name="$3" pid
  if alive "$pidfile" "$want"; then
    pid="$(cat "$pidfile")"
    kill "$pid" 2>/dev/null
    for _ in $(seq 1 20); do [ -d "/proc/$pid" ] || break; sleep 0.25; done
    [ -d "/proc/$pid" ] && kill -9 "$pid" 2>/dev/null
    echo "hilctl: stopped $name (pid $pid)"
  else
    echo "hilctl: $name not running"
  fi
  rm -f "$pidfile"
}

stop() {
  if have_systemd_units; then
    echo "hilctl: systemd units present — delegating to systemctl"
    systemctl --user stop hil-emulator.service
    return
  fi
  stop_one "$SHIM_PID" ble_elm.py     "shim"       # shim first: it is the TCP client
  stop_one "$ELM_PID"  elm_server.py  "emulator"
}

status() {
  if have_systemd_units; then
    for u in hil-elm hil-shim hil-emulator; do
      printf "  %-14s %s\n" "$u" "$(systemctl --user is-active "$u.service" 2>/dev/null)"
    done
    echo "  (health lives in the shim: systemctl --user is-active hil-shim)"
    return
  fi
  alive "$ELM_PID"  elm_server.py && echo "  emulator running (pid $(cat "$ELM_PID"))" \
                                  || echo "  emulator NOT running"
  alive "$SHIM_PID" ble_elm.py    && echo "  shim     running (pid $(cat "$SHIM_PID"))" \
                                  || echo "  shim     NOT running"
}

health() {
  local rc=0

  # Is the dash actually talking? The shim logs every frame in both directions,
  # so a growing log is the only real proof of a live link. An LE connection
  # alone is not: the dash can hold a stale link and send nothing, which is
  # exactly what happens if the shim restarted under it.
  if [ -f "$SHIM_LOG" ]; then
    local a b
    a=$(stat -c%s "$SHIM_LOG" 2>/dev/null || echo 0)
    sleep 3
    b=$(stat -c%s "$SHIM_LOG" 2>/dev/null || echo 0)
    if [ "$b" -gt "$a" ]; then
      echo "  link      OK — shim log grew $((b-a)) bytes in 3s"
    else
      echo "  link      IDLE — no traffic in 3s (dash not connected, or needs a hard reset)"
      rc=1
    fi
  else
    echo "  link      no shim log at $SHIM_LOG"; rc=1
  fi

  # bluetoothd once grew to tens of GB here because the shim rescheduled its
  # notify callback forever (a GLib source returning truthy repeats). That bug
  # is fixed, but the blast radius was the whole VM, so it stays watched.
  local btrss
  btrss=$(ps -o rss= -C bluetoothd 2>/dev/null | awk '{s+=$1} END {print s+0}')
  if [ "$btrss" -gt 512000 ]; then
    echo "  bluetoothd HIGH — ${btrss} kB RSS (>500 MB); restart it: sudo systemctl restart bluetooth"
    rc=1
  else
    echo "  bluetoothd OK — $((btrss/1024)) MB RSS"
  fi

  # A wedged shim spins. Healthy is a fraction of a percent.
  local pid cpu
  if have_systemd_units; then pid=$(systemctl --user show hil-shim.service -p MainPID --value 2>/dev/null)
  else pid=$(cat "$SHIM_PID" 2>/dev/null); fi
  if [ -n "${pid:-}" ] && [ "$pid" != "0" ] && [ -d "/proc/$pid" ]; then
    cpu=$(ps -o %cpu= -p "$pid" | tr -d ' ')
    if awk "BEGIN{exit !($cpu > 60)}"; then
      echo "  shim CPU  HIGH — ${cpu}% (>60%); it is spinning, restart it"; rc=1
    else
      echo "  shim CPU  OK — ${cpu}%"
    fi
  fi
  return $rc
}

case "${1:-status}" in
  start)   start ;;
  stop)    stop ;;
  restart) stop; sleep 1; start ;;
  status)  status ;;
  health)  health ;;
  logs)    tail -n "${2:-40}" "$SHIM_LOG" ;;
  *) echo "usage: $0 {start|stop|restart|status|health|logs [n]}" >&2; exit 2 ;;
esac
