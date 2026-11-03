# goldfish 资源与数据镜像

模拟器资源在 userdata FAT 镜像的 `data/` 下，挂载到 `/data` 后，应用从
`/data/data/{fonts,pet,tones}` 读取；示例书位于 `/data/book`。
板端的 `/resource` 分区是另一条路径，不能用它判断模拟器是否挂载成功。

常规 Make/CMake 构建只在 userdata 不存在时初始化；已有 `vela_data.bin` 保持原样。
资源更新与 FAT 修复必须在宿主停机后执行，不能对运行中的镜像执行 dd、mkfs 或 mcopy。
ROMFS system 镜像通过独立暂存文件生成后原子替换，不截断正在使用的文件。

V1 板端资源源目录是 `vendor/allwinnertech/lichee/board/r528s3/ynm3000_nand/data/res/`，
模拟器资源源目录是 `vendor/openvela/boards/vela/prebuilts/data/`；它们不会自动同步。
UI 新增图片若要在模拟器中可见，须把同一资源放到模拟器源目录，离线更新已有 userdata，
重编 goldfish 程序并重启客体。刷新 noVNC 网页只重连画面，不会替换客体程序或 userdata。
完成后核对运行中的客体文件与源资源哈希；用户禁止截图时不抓屏，视觉效果仍需用户确认。

## 宿主恢复入口

在宿主终端执行：

```bash
bash /data/dm/contest2026_070_arc/tools/repair_emu_resources.sh
```

该入口先检查没有其他 QEMU 实例，再调用当前运行管理脚本停机；复制 userdata 到暂存文件，
修复暂存 FAT、补齐资源，并逐文件核对 SHA-256。所有既有非资源文件必须保持相同内容。
成功后保留 `vela_data.bin.before-resources-时间` 备份，原子安装恢复镜像，再启动模拟器。
脚本只以新日志中宠物首帧非空作为资源加载信号；中文、图标、文件页操作仍需画面验收。

失败时保留原镜像，不忽略 fsck/mcopy 错误。运行管理脚本仍有宽泛 kill，入口会先拒绝
其他工作区的 QEMU；它尚未替代 P255 阶段 B 的进程管理改造。
隔离的 AI 会话无法观察宿主进程，恢复入口在该环境中主动拒绝修改镜像。

## 检查与复测

- 只读检查：`mdir -i <userdata> ::/data/fonts`、`fsck.fat -n <userdata>`。
- 挂载与资源内容分别判断：镜像有应用状态文件不代表字体/宠物目录仍存在。
- 离线候选通过 `prepare_data_image.py --stage-repair <新文件>` 生成，不会安装到活动路径。
- 工具测试：`python3 -B vendor/openvela/boards/vela/scripts/tests/test_prepare_data_image.py`。

实现：`vendor/openvela/boards/vela/scripts/prepare_data_image.py`，由该板的 CMake 与 Make 构建入口共用。
失败事件和验证边界见 [P266](../../devlogs/10-16-emu-resource-image-recovery.md)。
