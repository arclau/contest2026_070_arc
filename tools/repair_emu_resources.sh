#!/usr/bin/env bash
# Host-only recovery: stop the owned emulator, preserve userdata, seed and verify resources.
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
task_out="$task_root/cmake_out/vela_goldfish-arm64-v8a-ap"
python3 - "$task_root" <<'PY'
from pathlib import Path
import sys
init = Path('/proc/1/comm').read_text().strip()
if 'codex' in init or 'bwrap' in init:
    raise SystemExit('Please run this command in the host terminal, outside the isolated AI session.')
root = Path(sys.argv[1]).resolve()
expected_exe = root / 'prebuilts/emulator/linux-x86_64/qemu/linux-x86_64/qemu-system-aarch64'
for proc in Path('/proc').iterdir():
    if not proc.name.isdigit():
        continue
    try:
        name = (proc / 'comm').read_text().strip()
        if not name.startswith('qemu-system'):
            continue
        if (proc / 'cwd').resolve() != root or (proc / 'exe').resolve() != expected_exe:
            raise SystemExit(f'Another QEMU instance is present (PID {proc.name}); refusing broad stop.')
    except (FileNotFoundError, ProcessLookupError):
        continue
PY
bash /data/vela/run-emu.sh stop
python3 "$task_root/vendor/openvela/boards/vela/scripts/prepare_data_image.py" \
    --image "$task_out/vela_data.bin" \
    --data "$task_root/vendor/openvela/boards/vela/prebuilts/data" \
    --book "$task_root/vendor/openvela/boards/vela/prebuilts/book" --repair
bash /data/vela/run-emu.sh start |
    sed -E 's/([0-9]{1,3}\.){3}[0-9]{1,3}/[redacted]/g'
echo 'Resources repaired and emulator start requested; checking newly loaded pet frame.'
for ((task_attempt=0; task_attempt<60; task_attempt++)); do
    if rg -q 'f0=0x[1-9a-fA-F][0-9a-fA-F]*' /tmp/emu.log; then
        echo 'Pet resource frame loaded. Reconnect the browser and check Chinese text and the Files page.'
        exit 0
    fi
    sleep 1
done
echo 'Emulator started, but resource runtime verification is incomplete; inspect /tmp/emu.log.' >&2
exit 1
