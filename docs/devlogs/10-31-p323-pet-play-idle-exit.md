# P323 — 逗猫松手后自动结束

2026-10-31；V1 `ynm3000`，1920×1200。

用户要求去掉“结束逗猫”按钮，无人触摸时自然解除逗猫。
验收：按住手指可持续玩，松手后短暂等待自动恢复普通交互；短暂停顿可继续。

## 改动

仅改 `vendor/allwinnertech/apps/luncher_dm/pet_view.c`：

- 删除结束按钮及其点击回调；提示改为“按住空白处移动，松手片刻自动结束”。
- 进入逗猫及每次触摸事件记录触摸时刻；松手或失去触摸后 **3 秒** 自动退出。
- 期间重新按下继续玩；持续按住不会被此超时退出。
- 动画 timer 复用 `pet_play_stop(false)`，回收输入层并结束引擎 PLAY 状态。
- 沿用已有退出后的 5 分钟自由位置、疲劳终止和页面切换/销毁清理。

未改 V2、资源读取、缓存、CRC 校验及应用层重试。
修改前副本：`output/p323-pet-play-idle/before/pet_view.c`；保留原有未提交差异。

## 验证与产物

- 真实 LVGL + 实际 view/core/facade/renderer/PNG 的宿主交互测试，ASan/UBSan：5 组 PASS。
- 覆盖：移除按钮、2999/3000ms 边界、持续按住、重按续玩、松手不冒泡唤醒、
  PRESS_LOST、时钟回绕、退出后重进、suspend/reuse、销毁/重建。
- 测试与结果：`output/p323-pet-play-idle/host/test_idle.c`、`host/results.log`。
- 沙箱中 LeakSanitizer 因 ptrace 无法执行；获准在沙箱外运行后通过，未关闭检查器。
- 官方 `pack_deskmate.sh ynm3000` 编译及整包校验 PASS：配置/ELF 入口/启动/121 资源/整包。
- 首次打包的 dragon 被沙箱系统调用限制终止；获准重跑后生成有效整包。
- 有效日志：`output/p323-pet-play-idle/build-pack-host.log`。
- 镜像：`output/ynm3000/ynm3000.img`，MD5=`161436ac8c95387e8c22ff53465e18c1`。
- `vela.bin == nsh.fex`，SHA256=`775f1616ae8825675be2782947f095f1182a0fe64b23dec214023fe14e4f3f8d`。
- 本次未构建/运行 goldfish，未刷机；板端观感与操作待验，`hardware_test=NOT_RUN`。

## 交接

源码镜像导出前已备份 `output/p323-pet-play-idle/before/r528-mirror.tar`。
`sync_r528.sh` 已导出，本次应用目录一致；全局检查仍报 4 个历史 V2 映射源文件缺失。
文档按主源导出；文档检查仍有既有预算及孤立交接文件告警，不混入本次交互修订。
本次 Codex session 原始日志尚未入仓，LOGS=MISSING；串口常驻采集与对话日志分开判断。
不 commit/push/tag。

下一步：刷本次 V1 镜像，验松手 3 秒自动结束、期间重按可继续及普通桌面点击恢复。
