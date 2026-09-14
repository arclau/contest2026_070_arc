Books 子应用 (电子书阅读器) 实现方案
本方案旨在实现一个功能完善且易于操作的本地 txt 电子书阅读器，支持从 TF 卡读取书籍列表、分页阅读以及护眼模式。

⚠️ User Review Required / 需确认事项
IMPORTANT

书籍目录路径确认 我目前计划将扫描路径默认设置为 /mnt/sdcard/book。请确认在实际设备上 TF 卡的挂载路径是否为 /mnt/sdcard？如果路径不同（例如 /mnt/sd0），请在回复中指明。

WARNING

大文件内存限制 嵌入式设备使用 LVGL 渲染超大文本（例如 5MB 的 TXT）直接塞入 Label 会导致 OOM 或者极度卡顿。为了兼顾优雅的 UI 和系统性能，我计划采用**“流式分页阅读”**方案，每次只从文件中 fread 一屏的文本内容，不把全书加载到内存中。

💎 苹果级高定 UI/UX 设计规范 (Apple-like Premium Design)
为了确保界面达到“高端大气简约优雅”的要求，我将在设计中严格贯彻以下元素：

Glassmorphism (毛玻璃质感)：书架卡片和底部控制栏将采用 LV_OPA_70 透明度配合模糊效果（继承系统的 style_card），背景内容若隐若现。
Smooth Shadows (超柔和阴影)：每一本书的封面 (cover) 都会带上一层非常微弱、大半径的柔和弥散阴影 (LV_OPA_30)，增加 Z 轴的立体层次感。
Typography (极简排版)：
摒弃繁杂的边框和生硬的分割线，完全依靠留白 (Margin/Padding) 来区隔内容（使用 LV_FLEX_FLOW_COLUMN 与统一的 CARD_GAP）。
护眼模式的调色经过精心打磨（背景色：#F9F6F0 极浅羊皮纸，文字：#4A3F35 柔和深咖），比纯黑白对比度更低，视觉极度舒适。
Micro-interactions (微动画)：翻页或点击书籍时，会有类似 iOS 的平滑透明度过渡反馈。
Proposed Changes / 实施步骤
1. 书架数据层改造 (TF 卡目录扫描)
移除 deskmate_ui.c 中目前写死的 shelf[] 静态数组。
引入 <dirent.h>，编写 scan_books_directory() 函数，扫描 /mnt/sdcard/book。
将解析到的 txt 文件名填充到具备毛玻璃质感的圆角网格 (shelf_cont) 中，提取首字母作为精美的单色矢量封面。
2. 阅读器界面 (Reader UI) 开发
创建一个全新的全屏覆盖层 book_reader_overlay。
排版设计：
顶部沉浸式导航：返回按钮、极简书名，背景透明化处理。
内容区：全屏铺满的文字区，两侧留出充足的呼吸感边缘 (Margin)，不让文字顶到屏幕边缘。
底部悬浮控制台：圆角胶囊形状的半透明控制台，包含【上一页】、【下一页】和进度指示。
分页引擎：
维护文件偏移量 book_offset，点击翻页时进行固定字节读取并执行 UTF-8 边界安全截断防乱码。
3. 护眼模式 (Eye-Care Mode)
状态保存在 g_eye_care_mode 中。通过顶部一个极简的图标 (类似 iOS 的 TrueTone 太阳图标) 切换。
点击时实时触发生效并平滑重绘背景与文字颜色样式。
Verification Plan / 测试计划
UI 视觉审查：进入 Books 菜单，确认卡片拥有高级阴影与半透明材质，排版留白舒适。
分页与乱码测试：翻页 10 次确认无乱码。
护眼模式测试：点击护眼按钮，确认色彩切换柔和护眼。
防泄漏测试：反复进出阅读器界面，确认无内存泄漏。