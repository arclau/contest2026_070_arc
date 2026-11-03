# DesktopMate 桌面伙伴（YNM-3000）— R528 + openvela

> 2026 首届 openvela AI 硬件开发者大赛 ｜ 赛道：**AI 硬件产品创新** ｜ 专属仓 `contest2026_070_arc`
>
> 队长：arclau ｜ 协议：Apache-2.0（见 `LICENSE`）
>
> **当前交付基线（2026-11-02）**：V1 `ynm3000`，源码冻结于 [P364](docs/devlogs/11-02-p364-freeze.md)；板端镜像 53,149,696 字节，MD5 `ccb08b241d3170d5b9a43d4880ad8fbb`，`vela.bin == nsh.fex`。本仓提供 V1 源码、截至 P370 的 devlog 快照及筛查后的 AI Coding 会话日志。V2 为冻结副产品。

一句话：**openvela 大赛赞助的 R528 开发板 + 取自旧平板的 BOE 大屏**，逆向移植点亮后跑 openvela，做成放在桌上天天用的 AI 伙伴——陪伴（电子宠物）× 健康（在座提醒）× 好用（完整桌面 OS）。

---

## 一、作品简介

**openvela 大赛赞助的 R528S3-Gemini-S1 开发板** + **取自旧平板的 BOE 1200×1920 MIPI DSI 屏**（横屏 1920×1200，由安卓 DTB 移植点亮）+ GT9271 触摸 + UART 人体存在传感器 + 喇叭，运行 openvela（NuttX），做成桌面 AI 伙伴。
<img width="4032" height="3024" alt="IMG_1144" src="https://github.com/user-attachments/assets/de1bef04-36b1-4ffd-a4a4-aea49473fef2" />

**功能概览**（下图为 2026-09 的历史界面，当前 V1 UI 已更新）
<img width="1920" height="1200" alt="shot_20260912-080812-3" src="https://github.com/user-attachments/assets/fbbf1997-8c43-4146-8a4b-67f20f1191cb" />
<img width="1920" height="1200" alt="shot_20260912-080802-2" src="https://github.com/user-attachments/assets/8e4b66ea-1451-49f5-a517-95955416f18b" />

| 模块 | 功能 |
|---|---|
| 桌面 OS | 1920×1200 横屏、玻璃质感 Home 与锁屏、蓝色时刻壁纸、天气/健康卡、六应用 Dock |
| 电子宠物 | 18 组动作接入播放器；触摸、喂食、逗猫、自由行走与打盹；语音听/想/说时联动，状态持久化 |
| 日历 | 农历 + 二十四节气（太阳黄经逐点校验）+ 纪念日增删 |
| 闹钟 / 时间 | 多闹钟有序插入 + 贪睡 + 音色继承；自动对时总闸 + 无 RTC 落盘兜底 |
| 健康提醒 | LD2410B 毫米波在座检测；喝水 / 久坐分级提醒；每日统计；本地 23 个提示音 |
| AI 对话 | 按钮触发语音对话、AI 子页快问；MiMo 语音后端与设备端配 Key、流式回复和播报；口语化控制宠物、页面及设备功能 |
| 音乐 | XPlayer 本地 MP3/WAV 播放 + 控件 + 提示音/音乐/AI 三路音频仲裁 |
| 其他 | 电子书、文件管理、WiFi/蓝牙设置、UART 调试工具、显示（夜览 / 自动亮度）、实体按键截图 |

所有配置（WiFi/蓝牙/城市/宠物名/闹钟）均在设备本地 Settings 完成。

**这轮二次开发的主要变化**：Home/锁屏重做玻璃材质并加入壁纸；宠物从旧精灵展示扩展为可走动、打盹、互动且随语音起身的桌面伙伴；AI 语音接入 MiMo ASR/TTS、流式 LLM/TTS 与本地命令。天气图标重绘、默认城市和断网时的本地入口也作了整理。各项的代码、打包与板端验证范围见下方节点，不能把“已实现”一概当作“已实机验收”。

**当前验证边界**：冻结镜像已完成构建、打包及部分板端语音/宠物链路验证；仍有偶发 NAND 资源坏字节、音频 `xrun`，AI 回复偶发英语的原因未定。详见 [冻结记录](docs/devlogs/11-02-p364-freeze.md)，不要把历史截图当作当前 UI 验收图。

---

## 二、研发历程

### 第一阶段：硬件与桌面基础（2026-07-22～09-11）

项目从 BOE 大屏移植开始，逐步完成桌面、驱动和应用。下表记录第一阶段成果；2026-09 的固件版本仅作历史基线。

| 阶段 | 时间 | 主要工作 | 关键产出 |
|---|---|---|---|
| ⓪ 起点·点屏 | 07-22~07-28 | 用 `dts-to-vela-mipi` Skill 把 **RK3399 三防平板（1200×1920）的安卓 DTB** 提取/翻译为 R528 MIPI 面板驱动；注册 BOE 面板点亮；修 `Make.defs` 无条件编译 T070 致 `g_lcd0_config` 符号冲突（分辨率被锁 1024×600） | 屏幕点亮、分辨率正确 |
| ① 触摸打通 | 07-24~07-28 | GT9271 I2C 触摸：地址选择时序、零长度写/返回值判据等驱动 bug、外部上拉电阻；坐标上报 | 触摸可用（LVGL 可交互） |
| ② UI 基座 | 08-03~08-09 | 从 LVGL demo 演进为浅色 Dock 桌面；UI 三阶段拆分重构（`deskmate_ui.c` → `ui/` 7 页）；音乐播放链路 | 桌面 OS 骨架 + 音乐 |
| ③ 能力接入 | 08-10~08-23 | WiFi/蓝牙对接；UART 调试工具；LD2410B 在座检测；天气后台；豆包实时语音；宠物初版 | 六大 Dock 应用 + 宠物 |
| ④ 系统化 | 08-24~08-31 | 健康提醒（喝水/久坐）、语音编排、Settings 子页体系、稳定性排查 | 健康 + 设置体系 |
| ⑤ 大屏体验 | 09-01~09-09 | 日历（农历/节气）+ 闹钟 + 日期时间对时；宠物 11 态 × 12 帧重做；iPad 风格子页；全量汉化 | 完整产品形态 |
| ⑥ 收尾发布 | 09-10~09-11 | 稳定性/死代码清理、打包防呆、提示音尾音根治、版本型号、日志合规、报告 | **ok-20260911-21 / V1.0.0** |

### 第二阶段：V1 主线二次开发（2026-10-27～11-02）

| 阶段 | 主要工作 | 证据 |
|---|---|---|
| V1 主线确定 | V2 显示可用但拖动触摸断触，冻结为副产品；V1 `ynm3000` 继续交付 | [P289](docs/devlogs/10-27-p289-v1-mainline-ui-rework.md) |
| UI 与壁纸 | V1 接入 1920×1200 goldfish；Home/锁屏玻璃材质和蓝色时刻壁纸；壁纸改为常驻解码，避免每帧读 NAND 与解码 | [P290](docs/devlogs/10-27-p290-v1-sim-hookup.md)、[P348](docs/devlogs/11-02-p348-wallpaper-per-frame-decode.md)、[P359](docs/devlogs/11-02-p359-wallpaper-resident-chime.md) |
| 流畅度 | 定位壁纸重复解码与坏帧重试造成的 UI 卡顿；P348 对照板测中刷新轮次 p50 从 1361 ms 降至 17～20 ms，最终常驻壁纸方案尚无同口径复测 | [P346](docs/devlog.md)、[P348](docs/devlogs/11-02-p348-wallpaper-per-frame-decode.md) |
| 宠物重构 | 18 组动作真帧接入；悬浮胶囊、自由走动、闲置打盹、触摸/逗猫；语音 listen/think/speak 阶段起身并在播完后回睡 | [P303](docs/devlogs/10-30-p303-v1-pet-app-integration.md)、[P318](docs/devlogs/10-31-p318-pet-capsule-interaction.md)、[P319](docs/devlogs/10-31-p319-pet-tap-nap-freepos.md)、[P363](docs/devlogs/11-02-p363-pet-wake-on-speak.md) |
| AI 语音系统 | MiMo ASR/TTS 与设备端配 Key、流式回复；语音控宠物/页面/闹钟/场景；第一段异步播报与末段边收边放，板端测得句间衔接差 101 ms | [P330](docs/devlogs/11-01-p330-xiaomi-voice-settings.md)、[P331](docs/devlogs/11-01-p331-pet-voice-commands.md)、[P332](docs/devlogs/11-01-p332-voice-open-pages.md)、[P333](docs/devlogs/11-01-p333-alarm-scene-health.md)、[P362](docs/devlogs/11-02-p362-voice-gap.md) |
| 天气与本地能力 | 天气图标重绘、默认城市改北京；断网时本地快问/控制绕过联网门禁，网络错误有明确提示；语音识别仍依赖云端 | [P353](docs/devlogs/11-02-p353-offline-local-gate.md)、[P360](docs/devlogs/11-02-p360-weather-icons.md)、[P361](docs/devlogs/11-02-p361-city-beijing.md) |
| 启动与资源容错 | 缩短联网启动脚本，给宠物坏帧加缓存与上一帧兜底；NAND 偶发坏字节仍列为未闭环问题 | [P317](docs/devlogs/10-31-p317-pet-cache-and-fallback.md)、[P327](docs/devlogs/10-31-p327-wifi-boot-trim.md) |
| 冻结交付 | V1 APP、AI Agent 改动和 defconfig 镜像进本地专属仓；固件冻结，未提交或推送 | [P364](docs/devlogs/11-02-p364-freeze.md) |

逐节点状态见 [devlog](docs/devlog.md)；“代码已写”“打包通过”“板端已验”在节点中分别标明。

---

## 三、技术亮点与验证记录

| 方向 | 解决的问题 | 项目证据 |
|---|---|---|
| BOE 大屏与触摸 | 从旧平板 DTB 还原面板初始化及时序，完成横屏显示；适配 GT9271 的 I2C 上报与坐标映射 | `r528/vendor_drivers/`、[开发节点](docs/devlog.md) |
| 音频与语音 | 打通录音、ASR、LLM、TTS 与音频播放；多路声音统一编排，V1 后期优化句间衔接 | `r528/ai_agent/`、[P362](docs/devlogs/11-02-p362-voice-gap.md) |
| 电子宠物 | 动作帧播放、状态管理、交互和断电恢复；V1 后期接入语音阶段动作 | `r528/luncher_dm/`、[P303](docs/devlogs/10-30-p303-v1-pet-app-integration.md)、[P363](docs/devlogs/11-02-p363-pet-wake-on-speak.md) |
| 桌面 UI 与资源 | 1920×1200 V1 桌面、玻璃材质、资源校验与绘制优化 | [P290](docs/devlogs/10-27-p290-v1-sim-hookup.md)、[节点索引](docs/devlog.md) |


**硬件设计与适配**：项目涉及 R528 BSP、BOE MIPI DSI 面板、GT9271 触摸、LTR553 光感、音频编解码器和 LD2410B 人体存在检测；相关驱动镜像见 `r528/vendor_drivers/`，具体映射以 `contest2026_070_arc.xml` 为准。
**后续屏幕实验**：V2 1440×720 已能显示，触摸拖动断触，当前冻结；另一块 2160×1080 屏的异常原因未最终确定，不据此推断 R528 显示链路上限。

**openvela 能力运用（图形 / AI / 多媒体三项）**：LVGL 9.1（图形）、`packages/ai_agent` + 自研 `dm_ai` WS（AI）、XPlayer + PCM（多媒体）。组件：`nuttx`、`apps/graphics/lvgl`、`packages/ai_agent`、`apps/audio` + XPlayer。
**对 openvela 的改进（实测）**：① **已提 PR → [open-vela/vendor_allwinnertech#20](https://github.com/open-vela/vendor_allwinnertech/pull/20)**：`de_dsi` 超时、BOE 1200×1920 面板、`ltr553` ALS、`sun8iw20-codec` 麦克风关断 POP；② 通用建议（暂未单独提）：LVGL 三 fix（GE2D gating / 触摸物理分辨率 clamp / 缺字形占位）、音频示例统一补 `sw_params`。

---

## 四、目录结构

| 路径 | 作用 | 说明 / 编译树映射（`contest2026_070_arc.xml`） |
|---|---|---|
| `r528/luncher_dm/` | 桌面主应用（UI+宠物+AI+网络+天气+音乐） | 构建前覆盖到 `vendor/allwinnertech/apps/luncher_dm` |
| `r528/ai_agent/` | 冻结版 AI Agent 语音与对话改动（35 个文件） | 经 `copyfile` 映射到 `packages/ai_agent/...` |
| `r528/deskmate/` | 早期板级 defconfig 镜像 | manifest 仍映射到旧 `r528s3-gemini-s1`；与当前 V1 `ynm3000` 配置有 3 处路径差异，不能当作现行构建配置 |
| `r528/res_tones/` | 新增提示音 wav（23 个） | `lichee/board/common/data/res/tones` |
| `r528/vendor_drivers/` | 板级驱动源码镜像（BOE 面板 / GT9271 触摸 / ltr553 / codec / de_dsi / WiFi 重试） | 经 linkfile+copyfile 映射到 `vendor/allwinnertech/...` |
| `r528/agent_secrets.h.example` | AI 凭证模板（真文件永不进仓） | 手动 cp 到 `packages/ai_agent/include/agent_secrets.h` |
| `logs/` | **AI Coding 会话日志**（数量以 `manifest.json` 为准） | 作品提交要求 |
| `skills/` | Skill：`dts-to-vela-mipi` / `deskmate-ui` / `product-designer-ui` / `ai-devlog-system` / `deskmate-agent` / `devlog` / `contest-log-collector` | AI 开发证据 |
| `docs/devlog.md` + `docs/devlogs/` | 截至 P370 的节点索引和开发日志快照 | P370 之后的工作区 devlog 不再同步到专属仓 |
| `docs/specs/` | 语音/宠物/WiFi/书籍等设计规格 | AI 参与设计证据 |
| `docs/AGENTS.md` · `docs/methodology.md` · `docs/verify_checklist.md` | 会话交接协议 / 排障方法论 / 上板验证清单 | 过程证据 |

技术报告、演示视频和照片作为比赛材料另行整理，不在当前专属仓目录中。

---

## 五、运行方式

**现有完整开发工作区**中的 V1 构建入口如下。`pack_deskmate.sh` 会执行构建、打包和产物校验；冻结镜像的哈希见 [P364](docs/devlogs/11-02-p364-freeze.md)。

```bash
# 在现有完整 openvela 工作区根目录执行
./build.sh vendor/allwinnertech/boards/r528/ynm3000/configs/deskmate -j"$(nproc)"
bash vendor/allwinnertech/tools/pack_deskmate.sh ynm3000
```

专属仓 manifest 使用已有的 `r528s3-gemini-s1` 板配置。上游已有非空的 `apps/luncher_dm` 目录，因此同步 SDK 后需在工作区根目录覆盖应用文件，再编译：

```bash
cp -a contest2026_070_arc/r528/luncher_dm/. vendor/allwinnertech/apps/luncher_dm/
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/deskmate -j"$(nproc)"
```

旧板配置加应用覆盖的隔离编译已通过；**洁净 `repo sync`、打包与上板仍待验证**。专属仓尚未包含完整的 `ynm3000` 板目录与打包资源，不能用此流程宣称复现了 P364 的冻结固件。AI 凭证由使用者在本地配置，真凭证不入仓。

板端演示需准备音乐/电子书测试文件、网络和传感器；具体功能与未闭环问题以 [当前节点](docs/devlog.md) 为准。

---

## 六、AI Coding 使用说明

- 公开前筛查后保留 159 份、33,720 条事件；数量与来源以 `logs/arclau/manifest.json` 为准。
- 109 份敏感或不宜公开的会话已整份移出本地工作树；保留会话的旧工具载荷和敏感消息块已隐去。处理范围见 [P368](docs/devlogs/11-02-p368-public-session-review.md) 与 [日志说明](logs/README.md)。
- 移出的 109 份中有 102 份曾被本地旧分支跟踪。本 PR 候选从上游 `02bd7e0` 新建，避免带入该旧分支的额外历史；上游已有历史中的 8 份移出日志仍可从旧提交访问，普通 PR 无法清除它们。
- 需求拆解/方案设计、UI/驱动/语音编码、疑难调试（Data Abort/越界/矩阵旋转/音频）均与 AI 协作，人工 review + 上板验证；**新增沉淀 4 个开发期 Skill + 2 个运行时 Skill**（`deskmate-agent`/`devlog`）。
- 过程记录见 `docs/devlog.md`、`docs/devlogs/` 和 `logs/`，以当前文件与 manifest 为准。

---

## 七、提交与官方文档

- PR 自行 review 合入；首次贡献签 [CLA](https://openvela.com/#/community/cla)（PR 评论 `/check-cla`）。
- [大赛总览](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/contest_overview.md) ｜ [代码提交指南](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/code_submission_guide.md) ｜ [AI 日志手册](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)
