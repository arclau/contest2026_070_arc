# P305 — V1 现有帧：差异抚摸与手指逗猫

2026-10-30。用户授权做差异抚摸和逗猫；只用现有素材，不再生图。
目标：V1 luncher_dm，1920×1200；goldfish + ynm3000。保留已有未提交改动，不 commit/push/tag。
验收：摸头与摸肚子有不同反馈；手指逗猫能看、追、扑，松手停追；退出/挂起/重开不残留输入层。
按此前约定不启动/重启模拟器、不截图、不刷机；本节点区分宿主行为、编译打包与真实观感。

## 改动与操作

- 点头：现有 PET_NUZZLE + “蹭蹭”；点身体/肚子：NOTICE_USER + 小幅避让，不计抚摸奖励。
- 按住头 600ms：持续贴贴；按住后移动仍为抚摸；2200ms 后播 BLINK，再回持续贴贴，松手收势。
- 按住身体只作怕痒反馈；普通快速拖动和 800ms 双击入口保留；部位按可见缩放尺寸判断。
- 双击猫 → “喂食 · 玩耍”弹窗 → “逗猫玩”。空白处按住移动，蓝点标记目标；右上“结束逗猫”。
- 复用 NOTICE_USER/TURN/WALK/HAPPY/SETTLE，实现看、二维追手、短距离扑跳与落座；没有新增动作素材。
- 松手停止追逐；静止目标只扑一次，手指移开后才再次触发；边缘目标钳位且不隔空宣称抓到。
- 每 2 秒消耗 1 精力；精力≤15 或约30秒自动结束，疲劳后休息15秒；不足20精力/太饿/忙碌时不进入。
- 逗猫持续≥3秒只结算一次奖励；挂起、关闭、复用、模式退出均清理输入层；环境事件不打断互动。
- 门面新增 LVGL 主线程即时输入入口，保序消费；同一动作实例不重复刷新首帧，避免追逐时动作重置。

## 证据与状态

- 前态：`output/p305-pet-interaction/before/`；本轮差异与哈希见同目录 this-turn.diff / delivery.json。
- 82张PNG及manifest/revision与本轮开始逐文件SHA-256一致；SIM源资源同值；生成图片数量为0。
- 板资源已有4文件落后（2张已有PNG、manifest、revision），备份后同步现有源文件，未重生成图片。
- 引擎/播放器/缓存：`core-tests-final.log`，9组PASS，含抚摸分区/放松收势、追扑/计分/冷却回绕/精力。
- 真实LVGL+门面+引擎+播放器+PNG的无界面宿主：`host/final-results.log`，6组PASS，ASan/UBSan。
- 宿主覆盖：真实指针命中、双击入口、二维追逐/扑跳、松手停追、静止去抖、退出不唤醒锁屏。
- 生命周期覆盖：持续按下时超时清层、挂起/复用、活动模式销毁/重建；原始素材实际解码。
- goldfish：`goldfish-build-final.log`，21/21、10秒，编译通过；未启动客体，未更新运行中userdata。
- ynm3000：官方build.sh/pack_img.sh流程编译打包通过；完整资源/入口/启动/整包校验通过。
- 交付：`output/ynm3000/ynm3000.img`；manifest在同目录，硬件待验；vela.bin==nsh.fex。
- 固件MD5：`46a6eb57e336874b9d11d77d0febfedd`（09:36重打包，取代上一条`91e27138…`）；包哈希与源文件哈希见delivery.json。
- 重打包用官方`pack_deskmate.sh ynm3000`：应用层全量重编（含pet_view/pet_core/dm_pet）、libapps重归档、链接与打包一次通过，日志`output/p306-board-pack/pack-093442.log`。
- 重打包校验：`PASS ynm3000: config, ELF entrypoint, startup, 121 resources, nsh and whole image`；82张pet PNG与revision `b8239a6ce265bbbd`均在res.fex内。
- 首次链接失败：libapps.a含Oct29的pet_core/view `_2.o`与本次 `_1.o`重复；备份归档后仅移出旧2成员。
- 该唯一变量重试后链接通过；不改编译规则、不关闭检查。详情见archive-experiment.json与两个构建日志。
- 沙箱dragon被SIGSYS终止；已批准的宿主同流程打包通过，未将失败产物作为有效包。
- 原启动检查误拒绝`luncher_dm >/dev/log 2>&1 &`；只修校验器匹配，rcS不变，7种输入回归通过。
- 包校验：board-package-check.log / board-manifest.json；包装流程最后的归档步骤补齐到output/ynm3000。
- 板构建生成的config.h/arch移至本节点证据目录，恢复起始时不存在的状态，防后续ARM64头污染。
- 文档doc_check保留既有19项告警，docs单向导出后核对一致。
- 源码导出只读检查存在历史广泛漂移；为保留目标独有改动，未执行全量覆盖，镜像同步未闭环。
- 会话：Codex / cwd=/data/dm / `01a18c10-c727-7b13-bdc2-45fe0ac89d1b`；非零原始事件已核对。
- 共享转换器不兼容response_item，本次日志未入仓（LOGS=MISSING）；未修改采集器，未导出其他会话。

下一步：加载本次程序后用户复核触摸部位、追手速度与扑跳观感；有偏差只调整对应交互参数。
