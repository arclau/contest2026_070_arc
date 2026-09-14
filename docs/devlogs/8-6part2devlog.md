# DevLog 2026-08-06 (Part2) — 无声问题根治：writei 字节账目 + 24bit 位深传递

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**

---

## 一、任务背景

Part1（ok-20260806-14）遗留最高优先项：XPlayer 播放 MP3/WAV **writei 成功（wr>0）但完全无声**。声卡硬件已排除（aplay sine 有声），疑点集中在 XPlayer/dm_sound 对接层。本 Part 定位到两个确定性根因并修复，编译打包固化。

## 二、根因分析（代码级定位）

### 根因 1（主因）：dm_sound_write 返回值"帧数"被当作"字节数"使用

`audioRenderComponent.c writeToSoundDevice()` 循环：

```c
nWritten = SoundDeviceWrite(p->pSoundCtrl, pPcmData, nPcmDataLen);
if (nWritten > 0) {
    nPcmDataLen -= nWritten;          /* 按字节扣减 */
    pPcmData    += nWritten;          /* 指针按字节前移 */
    AudioDecCompReleasePcmData(p->pDecComp, nWritten);  /* 按字节释放解码缓冲 */
```

而 `deskmate_ui.c dm_sound_write()` 直接 `return wr;` —— `snd_vela_pcm_writei` 返回的是**帧数**（16bit 立体声 1 帧 = 4 字节）。例如 7680 字节 → frames=1920 → wr=1920 → 返回 1920，调用方以为只写掉 1920 字节，剩余 5760 字节从错位偏移继续写 → **数据被反复错位重写、解码缓冲释放账目错乱** → 声卡吃进的是碎片数据 → 静音。

参考实现 `soundControl_tinyalsa.c __SdNullWrite` 返回 `nDataSize`（字节），契约即为字节。

**修复**：`dm_sound_write` 返回 `wr * src_fb`（实际写入帧数 × 源格式每帧字节数）。

### 根因 2：adecoder 第二处把 nBitpersample 硬编码 16，24bit 位深被覆盖

`adecoder.c` 有两处填 `raw_data.nBitpersample`：第一处（770-837 行，ok-20260806-12 已修）按 `pBsInFor->bitpersample` 填 24/32/16；但**第二处（883-889 行，`!modeflag` 普通 PCM 重初始化路径）硬编码 `= 16`**，把 24bit WAV 的 24 覆盖回 16 → dm_sound 以 S16_LE 配置 + 24bit 数据 → 每帧错位 → 静音。日志中 `bitPerSample num change from 16 to 16` 恒 16 即由此而来。

**修复**：第二处也按 `pBsInFor->bitpersample` 填 24/32/16。

### 配套修正：S24_LE 语义不符，改为恒 S16_LE + 软件降转

aw-tiny-alsa 的 `S24_LE` 物理宽度 32（4 字节容器），与解码器 packed 24bit（3 字节/样本）不匹配，直接 S24_LE 配置仍会错位。而 aplay sine（S16_LE）是板上唯一验证有声的路径 → **声卡恒配 S16_LE，dm_sound_write 内把 24/32bit 源软件降转 16bit**（24bit 取高 16 位含符号扩展）。

## 三、改动清单

| 文件 | 改动 |
|------|------|
| `apps/luncher_dm/deskmate_ui.c` | ① `dm_sound_write`：返回值改为字节数 `wr*src_fb`（根因 1）；② 新增 24/32bit → 16bit 降转（静态 conv 缓冲）；③ `dm_sound_open`：恒 `SND_PCM_FORMAT_S16_LE` |
| `chips/r528/components/multimedia/src/libcedarx/decoding/adecoder.c` | 第二处 raw_data 初始化 `nBitpersample` 按 `pBsInFor->bitpersample` 填 24/32/16（根因 2） |

## 四、验证结果

- 编译：`./build.sh ... -j$(nproc)` ✅ 通过（无新增错误）
- 打包：`source envsetup.sh && lunch_nuttx 2 && pack` ✅ 成功
- 产物：`nsh.fex` 与 `vela.bin` **字节一致**（7478440 B）✅
- 镜像：`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`
- 固化：git_snapshot.sh 自动打 tag（见七）

### ✅ 上板验证（用户确认）

播放 IGNIS.wav（24bit WAV）**出声成功**！日志特征与修复前对比：

- 修复前：`writei` 尺寸 7680 后接 26/578/5/78 等错位碎片尺寸（帧数当字节数的账目错乱）
- 修复后：`writei size=7680 bits=16 ch=2 frames=1920 wr=1920` **连续稳定输出**（写满即完整消费，无碎片重写）
- 用户确认："终于出声音了" → 无声问题闭环

> 说明：日志中 `bits=16` 恒 16 属预期——修复方案就是声卡恒配 S16_LE，24bit 源在 `dm_sound_write` 内软件降转 16bit（取高 16 位），故日志仍显示 16。`needDirect 0→1` 重配仍在（checkSampleRate 检测到 direct 标志变化 → Stop/SetFormat/Start），但重配后正常出声，无实际影响。

## 五、遗留事项 / 下一步

1. **无声问题已闭环**（ok-20260806-16 修复 + ok-20260806-audio 上板验证通过）
2. SDMMC 多块读 DMA 根治（遗留）
3. 模拟器 freetype（遗留）
4. 其余 Part1 遗留项不变

---

*DevLog by AtomCode (deepseek-v4-flash)*
