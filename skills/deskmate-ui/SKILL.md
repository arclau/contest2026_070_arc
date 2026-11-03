---
name: deskmate-ui
description: DesktopMate LVGL UI 实现与审查。V1 主线为 1920×1200 横屏（luncher_dm，无模拟器）；V2 为 1440×720 冻结副产品（luncher_dm_v2，goldfish 预览）。两棵源码树独立，先确认改哪条线。
---

# DesktopMate UI

## 入口与边界

- 工程 `/data/dm`。**两条线，两棵树**：
  - V1 主线：`vendor/allwinnertech/apps/luncher_dm/`，板 `boards/r528/ynm3000`，逻辑画布 800×500 × DM_SCALE 2.4，宏在 `deskmate_ui.h`。
  - V2 副产品（冻结）：`vendor/allwinnertech/apps/luncher_dm_v2/`，板 `boards/r528/ynm3000_v2`，720×360 × 2.0，token 在 `dm_theme.h/.c`。
  - 两树不互相同步；改前先用路径和 `git status` 确认目标，禁止跨树复制补丁。
- 读取工程 `AGENTS.md`；构建与验收统一见 `project_docs/specs/misc/development-workflow.md`。
- 改布局前读**对应页面完整构建函数**、事件/销毁函数与共享父容器；用户已授权具体修复时直接实施，产品取舍才询问。
- V1 设计基准见 `project_docs/specs/ui/`（V1 规格待补）；V2 为 `home-layout-1440x720.md`。已有像素值/字体物理字号不能重复乘 `DM()`。
- V1 无模拟器路径，改 UI 需刷机验证；V2 用 goldfish。迁 V2 尺寸时再加载 `ui-port-1440`。

## 当前实现规则

1. 复用本线既有的 `deskmate_ui.*`、页面框架与图标源；不复制 UI 到 `/data/lv_port_linux` 或旧 examples。`sources.txt` 是 V2 公共 C 源清单。
2. 颜色、尺寸、透明度优先复用本线 token/材质函数。V2 用 `dm_theme_*`；V1 暂无主题层，改动时记录新增的硬编码值，为后续抽层留清单。
3. 图标优先复用项目 `deskmate_icons`、`dm_weather_icons` 或已启用 `LV_SYMBOL_*`。实际字体必须含所用字形并正确配置 fallback，不能假定符号自动存在。
4. 字体读 `init_fonts`、`FONT_*`、有效配置与资源路径。项目已有 FreeType 路径，禁止沿用“只支持 8/16/48”旧结论。
5. 布局完成后检查**父容器坐标系**、内容区、padding、滚动和裁切；容器局部坐标不能机械与屏幕全局比较。
6. 页面与 overlay 沿用当前导航；独立 screen 需要明确生命周期设计，不写成“LVGL 必崩”的通用禁令。
7. 回调签名必须匹配，用正确包装函数，禁止强转适配参数个数；UI 事件回调避免阻塞 I/O/sleep。
8. 页面删除前处理 timer、异步回调和持有的对象引用；重开不重复创建常驻实例。
9. 锁屏层级与输入、闲置时间重置按现有实现核对；不能只验证首次创建，要验证关闭/再开与锁屏唤醒。
10. LVGL 能力先查本项目版本源码、头文件和有效配置。是否启用 G2D、像素格式、stride 由目标决定，不从主屏尺寸推断 FB 格式。

## 验收

目标页显示与操作、中文/图标、弹窗/键盘、页面返回和重开；布局按父容器验证。goldfish 只验证 V2 已接入的 UI/stub 行为；V1 与所有硬件功能必须上 R528。按开发流程记录验证状态，未运行不能写通过。

旧 skill 存于 `project_docs/archive/v2/workflow-before-p254/deskmate-ui.md`，其中“唯一源码树”的结论已失效，仅作历史。
