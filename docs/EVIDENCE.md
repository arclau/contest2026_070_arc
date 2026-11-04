# 证据索引（EVIDENCE）

> 给评审与复核者的一份“从哪查、怎么查”的索引。本文件只做导航，不重复结论；
> 结论与边界以 [README](../README.md) 和 [devlog](devlog.md) 为准。

## 一、仓内有什么

| 路径 | 内容 |
|---|---|
| `r528/luncher_dm/` | 应用层源码（UI、宠物、健康、AI、音乐等） |
| `r528/deskmate/` | 早期板级 defconfig 镜像（见 README 中的口径说明） |
| `r528/ai_agent/` | 设备端 AI 链路与配置 |
| `r528/vendor_drivers/` | 面板 / 触摸 / 音频等驱动改动镜像 |
| `docs/devlogs/` | 按节点编号的开发记录（含失败与回滚） |
| `docs/specs/` | 规格、踩坑、流程文档 |
| `logs/` | AI Coding 会话原始日志 |
| `skills/` | 开发期使用的 Skill 定义 |

## 二、怎么构建

```bash
./build.sh vendor/allwinnertech/boards/r528/ynm3000/configs/deskmate -j"$(nproc)"
bash vendor/allwinnertech/tools/pack_deskmate.sh ynm3000
```

产物校验：`vela.bin` 与 `nsh.fex` 一致。

**限制（照实说）**：本仓 manifest 仍映射早期 `r528s3-gemini-s1` 板配置，**从洁净 `repo sync` 直接复现本轮 V1 固件尚未验收**。
上面的命令来自开发工作区，不能当作洁净检出可构建的证明。

## 三、验证证据在哪

| 证据类型 | 位置 | 说明 |
|---|---|---|
| 构建快照 | 本地开发仓 `ok-*` 标签（630 个 / 523 个提交） | 每次快照 = 一次构建验证，不在本仓 |
| 刷机与上板记录 | `docs/devlogs/` 各节点 | devlog 中 166 处刷机/烧录记录 |
| UI 实机与模拟器截图 | `docs/specs/ui/`、`docs/assets/` | 注意：历史截图不代表当前 UI，README 已注明 |
| 性能对照 | `docs/devlogs/11-02-p348-wallpaper-per-frame-decode.md` | 刷新轮次 p50 1361 ms → 17~20 ms（对照板测） |
| 冻结镜像哈希 | `docs/devlogs/11-02-p364-freeze.md` | 交付镜像可核对 |

## 四、AI Coding 日志怎么抽查

```bash
ls logs/arclau/ | head            # 按日期分目录
jq -r '.event' logs/arclau/*/*.jsonl | sort | uniq -c | head
```

共 159 份会话、33,720 条事件（以 `logs/arclau/manifest.json` 为准），记录了需求、尝试、失败与修正过程；不含任何凭据（导出时已脱敏）。

## 五、未闭环问题（不隐瞒）

| 问题 | 状态 |
|---|---|
| NAND / romfs 偶发坏字节 | 已止血（P317），未根治 |
| `reboot` 偶发挂死 | 需断电恢复，根因未定位 |
| AI 回复偶发英文 | 根因未定位，演示路径未复现 |
| 音频 `xrun` 偶发 | 未定位 |
| 返回键回归 / 音频关闭语义 | 已改，**未上板验证**，不宣称修复 |

## 六、文档导航

- [README](../README.md)：作品介绍、研发历程、技术亮点
- [devlog](devlog.md)：全部开发节点索引
- [方法论](methodology.md)：排障与验证方式
- [开发流程](specs/misc/development-workflow.md)：构建、打包、固化
- [已知坑](specs/misc/known-pitfalls.md)：踩过的坑与规避方式
