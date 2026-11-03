#!/usr/bin/env python3
"""Compile the actual local parser, without audio/network dependencies."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / 'packages/ai_agent/src/voice/voice_channel.c'
cases = [
    ('关掉音乐', 'music:pause'), ('关掉闹钟', 'alarm:off'),
    ('关掉声音', 'volume:0'), ('打开宠物设置', 'app:pet'),
    ('打开设置', 'app:settings'), ('打开音乐', 'app:music'),
    ('回桌面', 'app:home'), ('关掉', 'app:home'), ('关掉。', 'app:home'),
    ('去掉七点的闹钟', 'alarm:del:07:00'),
    ('删掉八点半的闹钟', 'alarm:del:08:30'),
    ('取消下午三点的闹钟', 'alarm:del:15:00'),
    ('取消闹钟', 'alarm:invalid'), ('删掉那个闹钟', 'alarm:invalid'),
    ('取消二十五点的闹钟', 'alarm:invalid'),
    ('删掉七点九十分的闹钟', 'alarm:invalid'),
    ('删掉七点几分的闹钟', 'alarm:invalid'),
    ('取消七点几的闹钟', 'alarm:invalid'),
    ('取消七点二十几分的闹钟', 'alarm:invalid'),
    ('取消七点7abc分的闹钟', 'alarm:invalid'),
    ('取消七点二十五分的闹钟', 'alarm:del:07:25'),
    ('把闹钟全删了', 'alarm:delall'), ('删除所有闹钟', 'alarm:delall'),
    ('把闹钟都删了', 'alarm:delall'),
    ('不要删除所有闹钟', ''), ('别取消七点的闹钟', ''),
    ('再睡五分钟', 'alarm:snooze'), ('贪睡', 'alarm:snooze'),
    ('我喝水了', 'health:water'), ('我喝了水', 'health:water'),
    ('我喝了一杯水', 'health:water'), ('我起来了', 'health:stand'),
    ('我站起来了', 'health:stand'), ('再等会', 'health:snooze'),
    ('我没喝水', ''), ('我还没喝水', ''), ('我要喝水', ''),
    ('我喝水了吗', ''), ('提醒我喝水', ''), ('我没有站起来', ''),
    ('我要站起来', ''), ('我喝水了没有', ''),
    ('晚安', 'scene:sleep'), ('我要睡觉了', 'scene:sleep'),
    ('我回来了', 'scene:home'), ('我出门了', 'scene:out'),
    ('我不出门了', ''), ('不要打开音乐', ''),
    ('安静点', 'volume:-10'), ('大声点', 'volume:+10'),
    ('听不清', 'volume:+10'), ('看不见', 'brightness:+10'),
    ('别出声', 'volume:0'), ('不要放了', 'music:pause'),
    ('别走了', 'pet:stop'), ('不想听这个', 'music:next'),
    ('阿强走路', 'pet:walk'), ('睡觉', 'pet:sleep'),
    ('阿强别走了', 'pet:stop'), ('我听不清', 'volume:+10'),
    ('不要别出声', ''), ('我没说别走了', ''),
]
import json
source = SOURCE.read_text()
parser = source[source.index('static int cn_digit('):source.index('/* Local execution and feedback')]
prefix = '''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s); if (n) snprintf(d, n, "%s", s); return len;
}
'''
rows = ',\n'.join('{' + json.dumps(a, ensure_ascii=False) + ',' +
                   json.dumps(b, ensure_ascii=False) + '}' for a, b in cases)
main = '''
int main(void) {
    const char *cases[][2] = {ROWS};
    int failures = 0;
    for (size_t i=0; i<sizeof(cases)/sizeof(cases[0]); ++i) {
        char cmd[64] = "";
        int hit = voice_local_cmd_parse(cases[i][0], cmd, sizeof(cmd));
        if (!hit) cmd[0] = 0;
        if (strcmp(cmd, cases[i][1])) {
            printf("FAIL %s: %s (expected %s)\\n", cases[i][0], cmd, cases[i][1]);
            ++failures;
        }
    }
    printf("%zu cases, %d failures\\n", sizeof(cases)/sizeof(cases[0]), failures);
    return failures != 0;
}
'''.replace('ROWS', rows)
with tempfile.TemporaryDirectory(prefix='dm-voice-test-') as td:
    c = Path(td) / 'parser.c'
    c.write_text(prefix + parser + main)
    exe = Path(td) / 'parser'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-g', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
