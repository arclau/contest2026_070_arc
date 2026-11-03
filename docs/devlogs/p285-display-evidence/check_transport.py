"""Execute extracted production DSI routines against RAM-backed registers.

No hardware I/O: delays/start and panel logging are stubbed. The packet builders,
ECC/CRC functions, register bitfields, basic/packet configuration, and panel init
are copied verbatim from the checked-out sources on every run.
"""
from pathlib import Path
import re
import subprocess
import sys

root = Path('/data/dm/vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/disp2')
out = Path(__file__).parent
ll = root / 'disp/de/lowlevel_v2x'
src = (ll / 'de_dsi.c').read_text()
panel = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'disp/lcd/BOE_1440x720.c'


def function(text, name):
    match = re.search(r'^(?:static )?\w+ ' + name + r'\([^;]*?\)\s*\{', text, re.M)
    assert match, name
    start = match.start()
    # These selected functions have balanced braces in comments/strings too.
    pos = text.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


names = ['dsi_ecc_pro', 'dsi_crc_pro', 'dsi_crc_pro_pd_repeat',
         'dsi_dcs_wr', 'dsi_gen_wr', 'dsi_basic_cfg', 'dsi_packet_cfg']
funcs = '\n'.join(function(src, n) for n in names)
fields = sorted(set(re.findall(r'panel->(\w+)', funcs)))
types = re.sub(r'^#include.*$', '', (ll / 'de_dsi_type.h').read_text(), flags=re.M)
enums = '\n'.join(re.findall(r'enum \w+\s*\{.*?\};', (ll / 'de_dsi.h').read_text(), re.S))
inc = (root / 'disp/de/include.h').read_text()
enums += re.search(r'enum disp_lcd_dsi_if\s*\{.*?\};', inc, re.S)[0]
text = panel.read_text()
table = text[text.index('struct hx_cmd {'):text.index('static void lcd_panel_init(u32 sel)\n{')]
init = function(text, 'lcd_panel_init')
code = '''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
typedef uint8_t u8;
typedef uint16_t u16, __u16;
typedef uint32_t u32, __u32;
typedef int32_t s32;
typedef uint64_t u64;
#define DISP_PRINT(...) ((void)0)
#define LOG_INFO 0
#define LOG_ERR 1
static void syslog(int level, const char *fmt, ...) {}
''' + types + enums + '\nstruct disp_panel_para {\n' + ''.join('u32 '+f+';\n' for f in fields) + '};\n'
code += '''static struct __de_dsi_dev_t regs;
static volatile struct __de_dsi_dev_t *dsi_dev[] = { &regs };
static u32 dsi_pixel_bits[] = {24,24,18,16}, dsi_lane_den[] = {1,3,7,15};
static u32 tcon_div = 4;
static unsigned captured, types_seen[64];
static unsigned char packets[64][128];
static unsigned sizes[64];
static int dsi_start(u32 sel, enum __dsi_start_t mode) {
    assert(mode == DSI_START_LPTX);
    assert(captured < 64);
    types_seen[captured] = regs.dsi_cmd_tx[0].dwval & 255;
    sizes[captured] = regs.dsi_cmd_ctl.bits.tx_size + 1;
    memcpy(packets[captured], (const void *)regs.dsi_cmd_tx, sizes[captured]);
    captured++;
    return 0;
}
static void dsi_delay_us(u32 us) {}
static void sunxi_lcd_delay_ms(u32 ms) {}
'''
code += funcs + '''
#define sunxi_lcd_dsi_dcs_write dsi_dcs_wr
#define sunxi_lcd_dsi_gen_write dsi_gen_wr
#define sunxi_lcd_dsi_dcs_write_0para(sel, cmd) dsi_dcs_wr(sel, cmd, NULL, 0)
''' + table + init
code += r'''
int main(void) {
    unsigned mismatches = 0;
    lcd_panel_init(0);
    assert(captured == 31);
    for (unsigned i = 0; i < captured; i++) {
        const struct hx_cmd *c = &hx_init_seq[i];
        unsigned expected = c->len == 0 ? 0x05 : c->len == 1 ? 0x15 : 0x39;
        if (types_seen[i] != expected) mismatches++;
        if (c->len > 1) {
            assert(packets[i][1] == c->len + 1);
            assert(packets[i][4] == c->cmd);
            assert(memcmp(packets[i]+5, c->data, c->len) == 0);
        } else {
            assert(packets[i][1] == c->cmd);
            if (c->len) assert(packets[i][2] == c->data[0]);
        }
        printf("cmd[%02u]=%02x len=%u DT=%02x expected_RK_DCS=%02x\n",
               i, c->cmd, c->len, types_seen[i], expected);
    }
    printf("payloads_pass=31 packet_type_mismatches=%u\n", mismatches);
    struct disp_panel_para p = {.lcd_x=720, .lcd_y=1440, .lcd_ht=800,
        .lcd_vt=1657, .lcd_hbp=50, .lcd_hspw=20, .lcd_vbp=17, .lcd_vspw=2,
        .lcd_dclk_freq=80, .lcd_dsi_format=0, .lcd_dsi_lane=4};
    for (unsigned mode=0; mode<=2; mode+=2) {
        memset(&regs, 0, sizeof(regs));
        p.lcd_dsi_if=mode;
        dsi_basic_cfg(0, &p);
        dsi_packet_cfg(0, &p);
        printf("mode=%u loop_n1=%u pixel_dt=%02x pixel_wc=%u hsa=%u hbp=%u hfp=%u vsa=%u vbp=%u vact=%u vt=%u eotp=%u\n",
            mode, regs.dsi_inst_loop_num.bits.loop_n1,
            regs.dsi_pixel_ph.bits.dt, regs.dsi_pixel_ph.bits.wc,
            regs.dsi_blk_hsa0.bits.wc, regs.dsi_blk_hbp0.bits.wc,
            regs.dsi_blk_hfp0.bits.wc, regs.dsi_basic_size0.bits.vsa,
            regs.dsi_basic_size0.bits.vbp, regs.dsi_basic_size1.bits.vact,
            regs.dsi_basic_size1.bits.vt, regs.dsi_basic_ctl0.bits.hs_eotp_en);
        assert(regs.dsi_inst_loop_num.bits.loop_n1 == (mode == 0 ? 49 : 4064));
        assert(regs.dsi_pixel_ph.bits.dt == 0x3e);
        assert(regs.dsi_pixel_ph.bits.wc == 2160);
    }
    return 0;
}
'''
(out / 'transport.c').write_text(code)
subprocess.run(['cc', '-std=gnu11', '-O0', '-g', '-fsanitize=undefined,address',
                str(out/'transport.c'), '-o', str(out/'transport')], check=True)
result = subprocess.run([str(out/'transport')], check=True, capture_output=True, text=True)
print(result.stdout, end='')
expected = int(sys.argv[2]) if len(sys.argv) > 2 else 0
assert f'packet_type_mismatches={expected}\n' in result.stdout
(out / ('transport-baseline.log' if expected else 'transport-fixed.log')).write_text(result.stdout)
