#!/usr/bin/env python3
"""Check V1 heap and all fixed framebuffer extents using production macros."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
VENDOR = ROOT / 'vendor/allwinnertech'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--config', type=Path, default=VENDOR / 'boards/r528/ynm3000/configs/deskmate/defconfig')
args = parser.parse_args()
config = args.config.read_text()
assert 'CONFIG_ARCH_BOARD_YNM3000_V2=y' not in config
if 'CONFIG_ARCH_BOARD_YNM3000=y' not in config:
    # The board Kconfig supplies this default; minimal defconfigs omit it.
    config += '\nCONFIG_ARCH_BOARD_YNM3000=y\n'
if 'CONFIG_R528_DDR_MAPOFFSET=' not in config:
    config += '\nCONFIG_R528_DDR_MAPOFFSET=0\n'
defines = '\n'.join('#define ' + key + ' ' + ('1' if value == 'y' else value)
                    for key, value in re.findall(r'^(CONFIG_\w+)=(.*)$', config, re.M))
dev_disp = VENDOR / 'chips/r528/drivers/rtos-hal/hal/source/disp2/disp/dev_disp.h'
buffers = int(re.search(r'^#define FB_NUM (\d+)$', dev_disp.read_text(), re.M)[1])
panel = (VENDOR / 'chips/r528/drivers/rtos-hal/hal/source/disp2/soc/BOE_1200x1920_mipi_config.c').read_text()
def property_value(name):
    return int(re.search(r'\.name\s*=\s*"' + name + r'".*?\.v.value\s*=\s*(\d+)', panel, re.S)[1])
width, height = property_value('fb0_width'), property_value('fb0_height')
with tempfile.TemporaryDirectory(prefix='dm-fb-memory-') as temporary:
    out = Path(temporary)
    (out / 'nuttx').mkdir()
    (out / 'arch/chip').mkdir(parents=True)
    (out / 'arch/board').mkdir()
    (out / 'nuttx/config.h').write_text(defines)
    (out / 'arch/chip/chip.h').write_text('')
    (out / 'arch/board/board_memorymap.h').symlink_to(
        VENDOR / 'boards/r528/ynm3000/include/board_memorymap.h')
    source = out / 'probe.c'
    source.write_text('''#include <stdio.h>
#undef __CONCAT
#include "r528_memorymap.h"
int main(void) {
    printf("%lu %lu %lu %lu %lu %lu\\n",
        (unsigned long)CONFIG_RAM_START,
        (unsigned long)(CONFIG_RAM_START + CONFIG_RAM_SIZE),
        (unsigned long)R528_DISP_DDR_MAPVADDR,
        (unsigned long)R528_DISP_RESERVED,
        (unsigned long)(R528_DRAM_PSECTION + R528_DDR_SIZE),
        (unsigned long)(R528_SAVE_LOG_DDR_MAPVADDR - R528_DSP_RESERVED));
    return 0;
}
''')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I' + str(out),
                    '-I' + str(VENDOR / 'chips/r528/hardware'), str(source),
                    '-o', str(out / 'probe')], check=True)
    start, heap_end, display, reserved, dram_end, dsp = map(
        int, subprocess.check_output([str(out / 'probe')], text=True).split())

frame = width * height * 4
regions = [('framebuffer', display, display + frame * buffers)]
for index in range(2):
    base = display + frame * (buffers + index)
    regions.append((f'rotation-{index}', base, base + frame))
print(f'heap=[{start:#x},{heap_end:#x}) display=[{display:#x},{display+reserved:#x})')
errors = []
if heap_end > display:
    errors.append('heap overlaps fixed framebuffer memory')
for name, base, end in regions:
    print(f'{name}=[{base:#x},{end:#x})')
    if end > display + reserved:
        errors.append(f'{name} exceeds display reservation')
    if end > dsp:
        errors.append(f'{name} overlaps DSP/log reservation')
    if end > dram_end:
        errors.append(f'{name} exceeds physical DRAM')
if errors:
    raise SystemExit('FAIL: ' + '; '.join(errors))
print('PASS: heap, framebuffer and both rotated frames are disjoint and within physical DRAM')
