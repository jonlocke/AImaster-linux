#!/bin/sh
set -eu

FIFO="/run/aimaster/input.fifo"
SCRIPT_BIN="/usr/bin/script"
AIMASTER_BIN="/usr/lib/aimaster/AImaster"

cleanup() {
  rm -f "$FIFO"
}
trap cleanup EXIT INT TERM

rm -f "$FIFO"
mkfifo -m 0600 "$FIFO"

# Open the FIFO read/write from the same process so the interactive CLI does
# not receive EOF immediately when no external writer is attached.
exec 3<> "$FIFO"
exec "$SCRIPT_BIN" -qefc "$AIMASTER_BIN" /dev/null <&3
