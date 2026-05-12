#!/usr/bin/env bash
# flash.sh — interactive USB-serial port picker for `pio run -t upload`.
#
# Why this exists: many shack ESP boards ship with Silicon Labs CP2102
# USB-serial chips, all with the same factory-locked default serial
# number "0001". Both macOS and Linux can only disambiguate them by
# enumeration order, so a hard-coded `upload_port` in platformio.ini
# silently targets the wrong board whenever two CP2102 devices are
# plugged in at once. This script lists the visible USB-serial devices
# and asks which one to flash.
#
# CP2102 EEPROM reprogramming was investigated (2026-05-12) — on the
# locked stock NodeMCU chips, vendor-OUT control transfers return
# success but the EEPROM is not actually written, so a per-board
# unique serial number is not achievable in software. See
# HANDOVER.md "Known gotchas" for the full story.

set -euo pipefail

PORTS=()
for p in /dev/cu.usbserial* /dev/cu.usbmodem* /dev/ttyUSB* /dev/ttyACM*; do
  [ -e "$p" ] && PORTS+=("$p")
done

case ${#PORTS[@]} in
  0)
    echo "[flash] no USB-serial ports found" >&2
    exit 1
    ;;
  1)
    PORT="${PORTS[0]}"
    echo "[flash] only port: $PORT"
    ;;
  *)
    echo "[flash] multiple ports — pick one:"
    PS3="[flash] choice: "
    select PORT in "${PORTS[@]}" "abort"; do
      case "${PORT:-}" in
        abort) echo "[flash] aborted"; exit 0 ;;
        "")    echo "[flash] invalid selection" ;;
        *)     break ;;
      esac
    done
    ;;
esac

echo "[flash] uploading to $PORT"
exec pio run -t upload --upload-port "$PORT"
