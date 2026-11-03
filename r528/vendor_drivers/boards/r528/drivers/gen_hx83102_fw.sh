#!/bin/sh
# Regenerate hx83102_firmware.c from the Himax 128KB blob.
# Usage: ./gen_hx83102_fw.sh <path-to-Himax_firmware.bin>
# The Firefly SDK blob lives at:
#   kernel-6.1/drivers/input/touchscreen/hxchipset/Himax_firmware.bin
# xxd -i emits a symbol derived from the filename; we rename it to a stable
# hx83102_firmware_bin[] / hx83102_firmware_bin_len so the driver can link.
set -e
if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <path-to-Himax_firmware.bin>" >&2
  exit 2
fi
SRC="$1"
DST="$(dirname "$0")/hx83102_firmware.c"
if [ ! -f "$SRC" ]; then
  echo "ERROR: firmware blob not found: $SRC" >&2
  exit 1
fi
xxd -i "$SRC" \
  | sed -e 's/^unsigned char [^[]*\[\]/unsigned char hx83102_firmware_bin[]/' \
         -e 's/^unsigned int [^ ]*_len =/unsigned int hx83102_firmware_bin_len =/' \
  > "$DST"
echo "Wrote $DST ($(wc -c < "$DST") bytes)"
