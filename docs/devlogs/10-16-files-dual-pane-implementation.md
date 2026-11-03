# P265 — 文件页双栏实现与验证

目标：1440×720 文件页布局清楚、触控操作可见，完整保留浏览、选择、复制/剪切、粘贴、新建和删除。
平台：共享 LVGL 源码；goldfish ARM64 编译，R528 编译打包，宿主离屏软件渲染与 POSIX 文件操作验证。
用户本轮授权采用审查后的设计，取代 P264 的重复顶栏、六钮浮坞方案。P264 保留为历史交接。

## 实现与验收现象

- 文件页沿用共享 overlay/状态栏，自己合并返回、标题、单行路径、上级、选择和更多菜单。
- 左位置栏 296px，间距 22px，右列表 1070px；导航 104px，文件行 112px。
- 宿主实际测量列表视口 `(344,186) 1070×508`，普通状态可显示四个完整条目。
- 选择态显示已选数量；复制/剪切/删除是带文字的玻璃圆钮，删除为红色。
- 操作区在右栏 flex 流中占位；列表视口随之缩小，最后一项可滚到操作区上方。
- 选择标记原位更新，整行接收点击；取消最后一项继续留在选择态，零选择禁用操作。
- 剪贴板卡显示待复制/待移动数量、粘贴、取消；新建/全选/刷新从更多菜单进入。
- 路径保留根位置和当前目录，深层目录通过省略菜单跳转；长名称省略，导航按钮不被挤出。
- 位置切换退出旧目录选择态；读取错误与空目录分开反馈。文件尾列显示真实大小。
- 确认弹窗有遮罩、清楚底色、足够按钮高度；删除与继续粘贴分别显示正确动作。
- 复制完成前保持 worker 忙碌状态，页面关闭/重开不允许并发重用剪贴板；不向其他 APP 弹完成提示。
- 规范化根目录拼接，阻止目录粘进自身；跨文件系统移动目录完成复制后递归移除源。
- 阅读器返回时尊重文件页自有导航，防止共享返回键重复出现。

## 代码边界与起点

所属仓 `vendor/allwinnertech`，起点 HEAD `20b1e1dc612d2a8788871cd207820267f53324dd`。
开始已有 8 个 dirty 文件，完整保留；本轮修改 `ui/ui_files.c`、`dm_theme.h`、
`deskmate_ui.c/.h`，新增 `tests/files_offscreen/`。本轮前文件备份位于 `/tmp/dm-files-p265/`。
`dm_theme.c`、音乐/日历/书籍/宠物的既有改动不是本轮新增，也未扩大这些页面的验收结论。

## 验证结果

| 项目 | 结果与证据 |
|---|---|
| 静态 | `git diff --check` 通过；工作区身份由 workspace_check 记录 |
| goldfish 编译 | 正规 `build.sh ...goldfish-arm64-v8a-ap --cmake -j8`；最终日志 `/tmp/dm-files-p265/build-sim-final.log` |
| 宿主 LVGL | 仓内 `tests/files_offscreen/run.py` 成功；日志 `/tmp/dm-files-p265/final-checks/checks.log` |
| 文件操作 | 复制/同名确认与改名、剪切、新建、删除取消/确认、深路径、长按后下一次点击通过 |
| 边界 | 零选择、后代粘贴拒绝、EXDEV 目录移动、复制中关闭重开、阅读器返回键归属通过 |
| 内容 | 4MiB 复制文件 SHA-256 一致，小文件逐字节一致 |
| R528 | `pack.sh` 构建、dragon 打包、check_res 分别通过；最终日志 `/tmp/dm-files-p265/pack-board-final.log` |
| goldfish 运行 | **未验**：当前会话无法连接宿主 X，新 X server 无法绑定 socket，启动器报告现有实例占用 |
| 板端运行 | **未验**：新屏显示、实际触摸与 G2D 观感仍需硬件 |

离屏测试直接编译当前文件页、主题与项目 LVGL，共享 helper 从源码提取；没有复制一套文件 UI。
测试的状态栏/媒体服务为宿主桩，截图不能作为 goldfish、阅读器/音频实际打开或 R528 运行证据。
复测入口：在 `/data/dm` 运行
`python3 vendor/allwinnertech/apps/luncher_dm/tests/files_offscreen/run.py`。

## 预览（宿主测试数据，非板端截图）

- [普通浏览](../specs/ui/files-1440x720/browse-native.png)
- [选择模式](../specs/ui/files-1440x720/selection-native.png)
- [剪贴板与粘贴](../specs/ui/files-1440x720/clipboard-native.png)
- [长路径](../specs/ui/files-1440x720/path-native.png)
- [删除确认](../specs/ui/files-1440x720/delete-native.png)

[离屏原始检查输出](../specs/ui/files-1440x720/offscreen-checks.txt) 与
[源码/配置/产物指纹](../specs/ui/files-1440x720/verification.json) 已保存；不依赖临时日志推断产物身份。

## 实验与环境边界

| 现象 | 单变量验证、结果与保留内容 |
|---|---|
| 首轮 ccache 只读 | 仅将 `CCACHE_DIR` 设为本轮 `/tmp` 子目录后构建通过；不改 SDK 配置 |
| 长按后的下一次点击被吞 | 控件重建改为原位选择状态更新，长按/释放/下一次进入目录复测通过；保留 |
| 弹窗透出背景文件名 | 仅文件页弹窗使用不透明白底、去除背景贴图；截图确认文字清晰；保留 |
| pack 整包含 nsh 检查失败 | 日志证实 32 位 dragon 收到 SIGSYS，整包时间仍是旧值；校验器正确拒绝旧包 |
| 32 位打包工具 | 用已有 `qemu-i386-static` 执行同一 dragon，打包成功；以导出 shell 函数重跑完整 pack.sh 后 check_res 全通过 |
| Make 后 CMake 取错头 | 实际预处理出现 ARM32 配置；Make 生成的 math.h 还遮蔽工具链数学头，导致 sinf/sqrt 声明缺失 |
| 头文件隔离 | 仅临时移开生成的 config.h、arch、math.h、float.h、setjmp.h，CMake 使用自己的头；构建后全部恢复 |

未关闭日志/断言/检查器，未清理原有用户差异，未更改外部 `/data/vela` 脚本，未打稳定 tag。
当前 SDK 原位生成头恢复为 R528 状态；今后切回 CMake 时仍需注意这个已实证的跨目标污染。

## 收尾与下一步

源文档和源码镜像由既有同步脚本单向导出。
当前 Codex session `01a144d0-82e8-7950-bf83-15f6269f660c` 未在 manifest/JSONL 中找到，
会话日志标 **MISSING**；未运行 MiMo 当日导出，未用手写总结替代原始日志。
仓内 logs 凭据模式扫描：0 命中、0 读取错误；没有自动提交或 push。
收尾静态检查仍提示源树/CMake 生成头并存、板端 defconfig 时间晚于 ELF；
前者已按实际预处理隔离验证，后者为构建末尾触碰且 Git 内容未改，不通过修改时间戳消除提示。
目标 ELF 头已核对为 ARM64/ARM32，运行结果依然单列。
文档检查开始已有一条历史交接未入索引警告（`10-14part1devlog.md`），本轮不改写该历史节点。
下一步：在能访问宿主显示/输入的会话中，启动本轮 goldfish ELF，实测文件页滚动、打开文件、
选择/粘贴、返回/重开；通过后才将 goldfish 运行项改为通过。硬件结果单列。
