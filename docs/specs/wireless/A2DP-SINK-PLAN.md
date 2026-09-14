# A2DP 蓝牙音箱交接包（给接手 AI，一读即开干）

> 状态：探坑完成（2026-09-10，P206），未实现。目标：手机连板子放歌（被人连/A2DP sink）。
> 现基线：`ok-20260910-12`；被人连半套（搜/配/连显）已在代码，无需重做。

## 一、背景（15 秒版）

- 设备：Allwinner R528S3-Gemini-S1，openvela（NuttX），BOE 横屏 + GT9271 + 喇叭（I2S）。
- 蓝牙：framework 小米 BT service（**bluetoothd 独立进程**，app 经 socket IPC 调用），ZBlue BREDR 栈，Realtek HCI。
  app 侧唯一入口：`vendor/allwinnertech/apps/luncher_dm/dm_net.c`（adapter/device 级），profile 层 app 侧零代码。
- 音频：`dm_sound_*`（`deskmate_ui.c`）吃 S16_LE PCM，默认 48k 立体声；声卡仲裁层 `dm_audio_fg_acquire/release/poll`（review-10）可挂起本地音乐。
- defconfig（`configs/deskmate`）：`CONFIG_BLUETOOTH_A2DP_SINK=y`、`CONFIG_BT_A2DP_SINK=y` 已开；
  **`CONFIG_AUDIOUTILS_TINYCOMPRESS` 未开，R528 芯片侧无 `/dev/audio/a2dpsnk`**——offload 路是死的，这是立项前提。

## 二、已探明链条（全是活的，勿重探）

```
手机 --A2DP--> bluetoothd(a2dp_sink_service, btservice.c:110 自注册)
  --> L2CAP --> a2dp_sink_packet_receive (sink/a2dp_sink_audio.c:167)
  --> queue --> audio_control_write (audio_interface/audio_control.c:350)
  --> audio_transport_write --> tinycompress /dev/audio/a2dpsnk  ★断点（设备不存在）
```

- SBC 流虚表：`a2dp_sink_stream_interface_t{repackage, packet_send_done}`，
  SBC 实现 `sink/a2dp_sink_sbc_stream.c`（只做 LATM 封装，不解码）。
- 解码器现成：`external/libfluoride-sbc`（Makefile+Kconfig 俱全，OI 标准 API：
  `OI_CODEC_SBC_DecoderReset/DecodeFrame`，见 `decoder/include/oi_codec_sbc.h`）。
- app 收状态现成：`bt_a2dp_sink_register_callbacks`（`framework/include/bt_a2dp_sink.h`，
  connection/audio/config 三回调），`dm_net.c` 照抄 adapter 回调注册模式即可。
- IPC 现状：`service/ipc/socket/` 只透状态不透音频（`bt_message_a2dp_sink.h` 无数据通道）。

## 三、最小改动方案（按顺序做，不跳步）

1. **软解 interface**（framework 新文件，不动状态机）：仿 `a2dp_sink_sbc_stream.c`
   写 `a2dp_sink_sw_stream.c`，`repackage` 内调 fluoride 解出 PCM（SBC 一般 44.1k 立体声），
   输出包改为 PCM 包；发送端 `send_done` 照抄 free。
2. **PCM 过进程**（最大块）：service→app 建 IPC 音频通道（仿现有 socket 控制通道，
   或共享内存环形缓冲；数据量 44.1k×16bit×2ch ≈ 1.4Mbps，socket 本地够用，先用 socket 别上 shm）。
   在 `a2dp_sink_audio_handle_timer` 处把包投往新通道，替代 `audio_control_write`。
3. **app 侧桥**（luncher_dm 新文件 `dm_bt_audio.c`）：收 PCM → 44.1k→48k 重采样
   （XPlayer 那套抖动大，自己写线性插值先跑通）→ `dm_sound_*` 写入；
   播前 `dm_audio_fg_acquire()` 挂起本地音乐，断开/暂停 `dm_audio_fg_release()`。
4. **UI**（`ui_bt.c`）：蓝牙子页加“音箱模式”开关 + 状态行（已连/播放中/断开）；
   开关 on = 确保 discoverable + 注册 sink 回调，off = 停回调不断 ACL。
5. **Kconfig**：`external/libfluoride-sbc` 选入 deskmate（`make menuconfig` 或改 defconfig，
   改法见 §五纪律 kconfig 项）；`TINYCOMPRESS` 不用开（走软解，不走 offload）。

## 四、验证清单（上板逐项）

1. 手机搜到板子 → 配对 → 显示已连接（半套，现有代码已支持，先验这步）。
2. 手机播歌 → 板子喇叭出声，无断续（看 44.1k→48k 质量）。
3. 本地音乐播放中 → 手机连上播歌 → 本地音乐被挂起；手机断开 → 本地恢复。
4. 来电/暂停/切歌（AVRCP 未接属正常，报告写清只做 A2DP 不做 AVRCP）。
5. 华为/小米/苹果三机各测一遍（各品牌 A2DP 行为有差异）。

## 五、仓库纪律（违反即返工，先读再动手）

- 工程根 `/data/dm`，配置 `configs/deskmate`（`nsh/` 勿动）；工具链 `./build.sh` 自动注入，
  勿 `make -C nuttx`。改 Kconfig 用 skill `kconfig-tweak` 语义匹配加载。
- 编译→打包→`check_res.sh`→`git_snapshot.sh` 全套（见 AGENTS.md §二），缺一步=没做完。
- `libapps.a` 同名多 `_N.o` 污染（P190）：打包前跑 `/tmp/dedup_ar.py`（若脚本丢失：
  同 stem 留归档内最新、删旧，`ar d` 后重链，二进制 `grep -a` 验新串）。
- 能读代码定位就别猜：现象→多假设→最小验证→结论（A3 教训：勿擅改驱动/栈）。
- UI 改动定位：`vendor/allwinnertech/apps/luncher_dm/`（Phase 1 拆分表见 AGENTS.md §一.5），
  `deskmate_ui.c` 不 include 自家头（用局部前向声明）。
- 敏感信息：key/号码/token 永不进 git；`agent_secrets.h` untracked + `.gitignore` 已配。
- 交作业映射只含 `r528/{luncher_dm,deskmate,res_tones}`：framework 改动走公共仓 PR
  到 `dev-ai-contest-2026`，不进专属仓（报告里写清）。

## 六、给接手 AI 的提示词（直接粘贴可用）

```
项目：/data/dm（openvela R528），目标：A2DP 蓝牙音箱（手机连板子放歌）。
先读：AGENTS.md → devlog.md → 本文件（project_docs/specs/wireless/A2DP-SINK-PLAN.md）。
现状：被人连半套已就绪（dm_net.c + ui_bt.c，基线 ok-20260910-12）；
A2DP 音频断在 audio_control_write → tinycompress（R528 无 a2dpsnk 设备）。
按 §三 1→5 顺序实现：①framework 软解 stream interface（fluoride，仿 sink_sbc_stream）；
②service→app PCM IPC 通道（仿现有 socket 控制通道）；③luncher_dm dm_bt_audio.c
桥接（44.1k→48k 重采样 + dm_sound + dm_audio_fg_仲裁）；④ui_bt.c 音箱开关；
⑤Kconfig 选入 fluoride。
纪律见 §五（编译打包 check_res snapshot + ar 去重 + 敏感信息禁入仓）。
每步先小验证再往下走；手机联调华为/小米/苹果各测；AVRCP 不做。
```

## 七、风险与回退

- 最大风险：手机 A2DP  quirks（编码协商/SBC 参数）+ IPC 音频通道抖动。先单机（1 台小米）跑通再扩机型。
- 回退：framework 改动全在新增文件，`git checkout` 即回；app 侧 `dm_bt_audio.c` 独立文件，
  UI 开关默认 off，不影响现有半套演示。
- 若 9.20 前未完成：半套 + 本文件即交付物，报告"未来工作"引用本文（探坑结论本身是技术难度证据）。
