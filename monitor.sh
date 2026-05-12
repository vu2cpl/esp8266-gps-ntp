#!/usr/bin/env bash
# monitor.sh — interactive USB-serial port picker for `pio device monitor`.
# Same picker as flash.sh; different action. See flash.sh for the why.

set -euo pipefail

PORTS=()
for p in /dev/cu.usbserial* /dev/cu.usbmodem* /dev/ttyUSB* /dev/ttyACM*; do
  [ -e "$p" ] && PORTS+=("$p")
done

case ${#PORTS[@]} in
  0)
    echo "[monitor] no USB-serial ports found" >&2
    exit 1
    ;;
  1)
    PORT="${PORTS[0]}"
    echo "[monitor] only port: $PORT"
    ;;
  *)
    echo "[monitor] multiple ports — pick one:"
    PS3="[monitor] choice: "
    select PORT in "${PORTS[@]}" "abort"; do
      case "${PORT:-}" in
        abort) echo "[monitor] aborted"; exit 0 ;;
        "")    echo "[monitor] invalid selection" ;;
        *)     break ;;
      esac
    done
    ;;
esac

echo "[monitor] connecting to $PORT"
exec pio device monitor --port "$PORT"
