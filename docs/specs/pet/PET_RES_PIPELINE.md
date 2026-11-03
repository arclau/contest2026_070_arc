# P5 宠物 PNG → res 分区 资源流水线清单

> 源帧：`project_docs/specs/pet/catpng/`（144 帧，12 动作 × 12 帧）  
> 目标：进固件 res 分区，板端可读。`check_res.sh` 只保证 res.fex 打进镜像，**不管帧完整性**，本清单补这个洞。

## 流水线步骤

1. **帧规范核对**（改帧后必做）
   - [ ] 命名：`pet_cat_<action>_<NN>.png`，action ∈ {idle,happy,sad,hungry,eat,play,groom,touch,walk,sleep,curious,celebrate}
   - [ ] 每动作 12 帧（NN=00..11），共 144 张；缺帧会动画跳变
   - [ ] 尺寸/透明通道与 `PET_SPRITE_SPEC.md` 一致
   - [ ] `md5sum` 变更表记录（便于回溯哪次改了帧）

2. **进 res 源目录**
   - [ ] 拷入 `vendor/allwinnertech/lichee/board/common/data/res/pet/`（或当前 res 布局约定路径）
   - [ ] 确认 res Makefile/清单包含新目录（漏清单 = 打包静默丢资源）

3. **编译打包**
   - [ ] `bash vendor/allwinnertech/tools/pack_deskmate.sh {ynm3000|ynm3000_v2}`（产物校验见 `manifest.json`）
   - [ ] 若改的是 prebuilt：先 `distclean` 再重编（坑 #4）

4. **板端验证**（烧整包镜像后）
   - [ ] `ls /resource/pet/`（或实际路径）帧数 = 144
   - [ ] 抽查 3 个动作首尾帧能打开
   - [ ] UI 播放动画无缺帧/闪烁

5. **固化**
   - [ ] `bash /data/vela/git_snapshot.sh`
   - [ ] `bash /data/vela/sync_r528.sh`（若 r528 镜像含 res 相关改动）
   - [ ] devlog 记节点（结论一行即可）

## 与 check_res 的边界

| 校验 | check_res.sh | 本清单 |
|------|:------------:|:------:|
| res.fex 是否打进镜像 | ✅ | — |
| nsh/vela 一致性 | ✅ | — |
| 帧是否齐全/命名正确 | ❌ | ✅ |
| 资源清单是否收录 | ❌ | ✅ |
