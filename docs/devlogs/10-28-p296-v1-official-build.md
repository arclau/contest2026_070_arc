# 10-28 P296 — V1 官方流程全量重编并上板

> 节点：**P296** ｜ 平台：BOARD（V1 `ynm3000`）｜ 结论：**官方编译打包流程可用，板端开机进桌面**

## 目标

按 2026-09-11 基线（`ok-20260911-21`）当时的编译方式——SDK 官方 `build.sh` + lichee 官方 `pack_img.sh`——对 V1 做 distclean 全量重编，不再依赖项目包装脚本。

## 命令与结果

```bash
cd /data/dm
./build.sh vendor/allwinnertech/boards/r528/ynm3000/configs/deskmate distclean   # rc=0
./build.sh vendor/allwinnertech/boards/r528/ynm3000/configs/deskmate -j$(nproc)  # rc=0，全量重编
cd vendor/allwinnertech/lichee
bash tools/scripts/pack_img.sh -c sun8iw20p1 -p rtos -b r528s3-ynm3000 \
  -o nuttx -d uart0 -s none -m normal -w none -v none -i none \
  -t "$PWD" -f r528s3/ynm3000_nand -g r528s3/ynm3000_nand                    # rc=0，pack finish
```

- 编译日志：`output/ynm3000/build-official-20261028015739.log`（1218 行，从 kernel 起全部 CC）
- 打包日志：`output/ynm3000/pack-official-2026102802.log`
- 产物：`lichee/out/r528s3/ynm3000_nand/rtos_nuttx_r528s3-ynm3000_uart0_128Mnand.img`，54,758,400 B，md5 `a80605fe43f30385ec84dc668b510ecb`
- 归档副本：`output/ynm3000/official-20261028/`（img / vela.bin / nuttx.elf / config / res.fex / md5.txt）

## 自检

| 项 | 结果 |
|---|---|
| `vela.bin` vs `nsh.fex` | md5 一致 `b4018764429f4aab8414021ba4dfe039` |
| `.config` 关键项 | `CONFIG_LUNCHER_DM_APP=y`、`CONFIG_LCD_SUPPORT_BOE_1200X1920=y`、`CONFIG_GT9271_IIC_TOUCH=y` |
| res.fex | 现做自 `board/r528s3/ynm3000_nand/data/res`，含 `wallpaper/blue-hour.png` |
| `boot0 checksum fail` | 10-27 打包日志同样存在，非本次引入 |
| 板端 | 用户确认烧录后开机进桌面（2026-10-28） |

## 坑：lunch_nuttx 不能用于本项目

`lunch_nuttx` 菜单里本项目名是 `ynm3000`（无 `r528s3-` 前缀），`pack()` 由 `RTOS_PROJECT_NAME%_*` 推出 `-b ynm3000`：

- `pack_img.sh:1501` 的 128M nand 特判只认 `r528s3-ynm3000`，传 `ynm3000` 会退回 256M 布局；
- `RTOS_TARGET_PROJECT_PATH=ynm3000_nand` 缺 `r528s3/` 前缀，板级配置路径也对不上。

因此固定用法：**直接调 `pack_img.sh` 并显式 `-b r528s3-ynm3000`**（与官方 `pack.sh` 对其它板型的写法一致）。

## 注意

- `distclean` 后 `.config` 由 defconfig 重建，此前在 `.config` 里手改的选项会丢失。
- `pack_deskmate.sh` 内部调用同一 `pack_img.sh`，另加 manifest 校验；保留为一键包装，不是另一套打包逻辑。
- 本次未跑 `check_deskmate.py`（需要 manifest 阶段产物），资源/哈希改为上表逐项核对。
