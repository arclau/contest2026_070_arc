#!/usr/bin/env python3
"""Compile production pet engine/player/cache under ASan+UBSan with isolated UI/IO."""
import json
import subprocess
import tempfile
import zlib
from pathlib import Path
from PIL import Image

ROOT=Path(__file__).resolve().parents[4]
APP=ROOT/'vendor/allwinnertech/apps/luncher_dm'
HERE=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='dm-pet-actions-') as temporary:
    out=Path(temporary)
    for d in ['nuttx','lvgl','netutils','assets']:(out/d).mkdir()
    (out/'nuttx/config.h').write_text('')
    (out/'netutils/cJSON.h').symlink_to(ROOT/'apps/netutils/cjson/cJSON/cJSON.h')
    (out/'lvgl/lvgl.h').write_text('''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
typedef void lv_obj_t;
typedef int lv_coord_t;
typedef struct { unsigned magic,cf,flags,w,h,stride; } lv_image_header_t;
typedef struct { lv_image_header_t header; uint32_t data_size; const uint8_t *data; const void *reserved,*reserved_2; } lv_image_dsc_t;
#define LV_IMAGE_HEADER_MAGIC 0x19
#define LV_COLOR_FORMAT_ARGB8888 1
uint32_t lv_tick_get(void);
uint32_t lv_display_get_inactive_time(void *display);
void lv_image_cache_drop(const void *src);
static inline void test_log(const char *fmt,...) { (void)fmt; }
#define LV_LOG_USER(...) test_log(__VA_ARGS__)
''')
    manifest=json.loads((APP/'assets/pet/manifest.json').read_text())
    crc=[]
    for asset in manifest['assets']:
        source=APP/'assets/pet'/asset['file']
        (out/'assets'/asset['file']).symlink_to(source)
        with Image.open(source) as im:
            raw=im.tobytes('raw','BGRA')
        # Byte reversal is an independent expected-value calculation, not an image edit.
        mirrored=b''.join(b''.join(raw[(y*320+x)*4:(y*320+x+1)*4] for x in range(319,-1,-1)) for y in range(320))
        crc.append((zlib.crc32(raw),zlib.crc32(mirrored)))
    (out/'assets/revision').write_text(manifest['revision']+'\n')
    (out/'expected_crc.h').write_text('static const unsigned long expected_crc[][2]={\n'+''.join('{%du,%du},\n'%pair for pair in crc)+'};\n')
    flags=['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-g','-O1','-fno-omit-frame-pointer',
        '-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-Wno-sign-compare','-Wno-misleading-indentation',
        '-I'+str(out),'-I'+str(APP)]
    jobs=[('core',[HERE/'core.c',APP/'pet_actions.c',ROOT/'apps/netutils/cjson/cJSON/cJSON.c'],[]),
          ('assets',[HERE/'assets.c',APP/'pet_assets.c',APP/'pet_actions.c'],['-DPET_ASSET_ROOT="'+str(out/'assets')+'"','-lpng','-lz'])]
    for name,sources,extra in jobs:
        executable=out/('test-'+name)
        subprocess.run(flags+[str(p) for p in sources]+extra+['-o',str(executable)],check=True)
        subprocess.run([str(executable)],check=True)
