# WiFi / 蓝牙 UI 接入与嵌入式 BSP 架构设计方案 (wifijianyi.md)

> **文档定位**：本文件是对 `wifidev.md` 需求的全面升级与回应。从**专业嵌入式 BSP 开发者**的视角，不仅实现了“手机式”的顺滑交互，还深入考虑了 R528 平台底层的**异步并发、存储防掉电、进程间通信(IPC)鲁棒性及射频功耗管理**。
> **目标**：为执行代码编写的 AI 提供一套**绝对安全、防崩溃、防内存泄漏**的设计蓝图与编码紧箍咒，同时提供完全符合现有 iOS-style 视觉规范的 UI 组件组装草图。

---

## 一、 系统级诊断与根本性修复

在处理“点击WIFI进不去”的问题时，不能只看表面，需要从事件与生命周期双向发力：

1. **事件冒泡误杀机制**：
   - **问题**：目前通过 `lv_event_get_target(e) != lv_event_get_current_target(e)` 拦截事件，在复合控件中会导致点击空白区域也被误判拦截。
   - **BSP 级修复**：不依赖 target 拦截。让 Switch 自身捕捉并消耗（`lv_event_stop_bubbling`）它的 `CLICKED` / `VALUE_CHANGED` 事件，Row 整体保留干净的 `CLICKED` 响应。
2. **幽灵定时器与野指针 (致命错误)**：
   - **问题**：UI 销毁时如果底层轮询 timer 还在跑，下一次轮询必报 Data Abort（段错误）。
   - **BSP 级修复**：在 `create_wifi_subpage` 创建 `wifi_timer` 时，必须利用 LVGL 的**对象析构回调**（`LV_EVENT_DELETE`）去销毁 Timer，彻底告别野指针。

---

## 二、 WiFi 模块 BSP 级架构与交互设计

作为手机级体验，不仅要求 UI 好看，更要求底层逻辑严密无缝。

### 1. 射频 (RF) 与状态机管理
- **功耗意识**：WiFi 扫描（`wifi_scan_networks`）极其耗电。退出 WiFi 界面时，必须停止扫描逻辑。
- **状态机跃迁**：引入明确的连接状态机 `IDLE -> SCANNING -> CONNECTING -> DHCP_WAITING -> CONNECTED -> DISCONNECTING`。UI 按钮的启用/禁用均由此状态机驱动。

### 2. 交互与网络配置持久化 (防掉电)
- **获取 IP**：不能仅靠状态标志，必须调用 Socket `ioctl(SIOCGIFADDR)` 探测 `wlan0` 网卡的真实 IPv4 地址。
- **配置持久化与原子写入（防变砖）**：
  - **BSP 级规范**：保存密码至 `/data/etc/wifi/wapi.conf` 时，必须使用**原子写入（Atomic Write）**方案：先写入 `wapi.conf.tmp`，执行 `fsync()` 落盘后，再 `rename("wapi.conf.tmp", "wapi.conf")`。

### 3. 阻塞式驱动的解耦机制
- `wifi_connect` 最长阻塞 60 秒。连接必须放到 `pthread` 后台线程。
- **连接代数 (Generation)**：UI 取消时增加 `connection_generation`。后台线程醒来发现代数不匹配，自动丢弃连接结果，防止旧连接结果覆盖新状态。

---

## 三、 蓝牙模块 BSP 级架构与 IPC 设计

### 1. IPC 鲁棒性与自动恢复
- **断线重连**：每次 UI 查询时如果发现 `bluetoothd` Socket 断开，必须有尝试 `bluetooth_create_instance()` 重新建连的机制。

### 2. 手机级蓝牙 UI 功能定义
- **可被发现开关 (Discoverable)**：提供勾选框，勾选后开启可见性并设定倒计时自动关闭。
- **设备扫描与队列 (Discovery & Paired)**：扫描回调中的发现结果需要加入**去重哈希表/数组**，根据 Mac 地址去重后再通知 UI 刷新。
- **异步配对请求握手**：收到 `on_pair_request` 信号时，弹窗询问用户是否配对，点击接受后调用 `bt_device_pair_request_reply(accept=true)`。

---

## 四、 ⛔ 核心 BUG 防范与防御性编程指南 ⛔ (AI 铁律)

> 本章是系统稳定性的最后防线。违规将导致板卡死机、花屏或内存泄漏。

### 1. 多线程与竞态灾难防范 (Thread Safety & Race Conditions)
- **UI 单线程绝对铁律**：WiFi (RTW) 和 BT (Socket) 的回调中，**绝对禁止出现任何 LVGL 渲染函数的调用**。
- **唯一合法路径**：回调只允许更新静态缓存（加锁 `pthread_mutex_t` 或无锁 `volatile` 状态），然后通过 UI 的 `lv_timer` 将数据拉取到屏幕上。

### 2. 内存碎片与泄漏控制 (Memory & OOM Prevention)
- **析构闭环**：凡是 `lv_obj_create` 出来的动态遮罩或弹窗，必须在离开此状态的所有逻辑分支中写上 `lv_obj_del` 并**立即置指针为 NULL**。
- **避免频繁 malloc**：重建 WiFi 列表时，不要盲目 `lv_obj_clean` 全量摧毁重建，尽量复用现有列表项对象以防 OOM。

### 3. UI 越界与模态穿透防御 (UI Alignment Protection)
- **动态内容截断**：长 SSID / 蓝牙名称必须使用 `lv_obj_set_width(label, LV_PCT(100))` 和 `lv_label_set_long_mode(label, LV_LABEL_LONG_DOT)` 强行做省略号截断。
- **全屏模态穿透防御**：密码键盘背景遮罩必须添加 `lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE)` 但不绑定回调，以吸收背后的点击，防止重入。

---

## 五、 🔧 驱动 API 对接强制规范 🔧 (防误调、防瞎写)

> 针对 AI 在调用全志 R528 及 RTW8733BS 底层接口时常犯的“顾名思义”错误，特此下达 API 级别的硬性调用约束。

### 1. WiFi 驱动调用时序与传参死线 (`wifi_conf.h`)
- **`wifi_scan_networks()` 陷阱**：
  - 传参中的 `user_data` 在回调结束前必须保持生命周期有效，严禁传入局部变量指针。如果不需要传，老老实实传 `NULL`。
  - 回调函数 `dm_net_scan_handler` 处于 RTW 任务上下文，里面绝对不能有延时 (`sleep`/`msleep`) 或锁死逻辑。
- **`wifi_connect()` 鉴权参数陷阱**：
  - WPA2_PSK 密码必须是 **8~64 字节** 的字符串。调用前必须做 `strlen` 校验，长度不符直接 UI 拦截，严禁下发驱动导致 Driver Assert 崩溃。
  - 对于无密码的开放网络 (OPEN)，第二个参数必须是 `RTW_SECURITY_OPEN`，且 `psk` 传 `NULL`，`psk_len` 传 0。

### 2. 蓝牙 驱动 IPC 调用防错 (`bt_adapter.h`)
- **指针判空 (Null Pointer Exception)**：
  - 蓝牙所有的 API 的首个参数都是 `bt_instance_t *`。在调用前，必须执行 `if (!g_bt_ins) return;` 的安全拦截。严禁直接把未实例化的野指针塞进底层 API。
- **状态一致性**：
  - `bt_adapter_enable()` 后，必须等待状态变为 `BT_ADAPTER_STATE_ON` 后再调用 `start_discovery`，严禁抢跑。
- **配对交互的回调处理**：
  - `on_pair_request` 的 `Mac 地址` 必须深拷贝保存，严禁直接存底层回调过来的临时指针。

---

## 六、 🚀 落地操作代码片段与范式 (API Cheat Sheet) 🚀

为了让大模型直接落地，提供以下标准封装范式（必须照搬此思路，严禁魔改）：

### 1. WiFi 异步扫描与安全回调范式 (`dm_net.c`)
```c
// [回调层] RTW 线程执行，仅做数组填充
static rtw_result_t safe_scan_handler(rtw_scan_handler_result_t *r) {
    if (!r) return RTW_SUCCESS;
    if (r->scan_complete) {
        g_scan_done = 1; // 仅标记完成
        return RTW_SUCCESS;
    }
    // 数组边界拦截防御
    if (g_ap_count >= DM_NET_AP_MAX) return RTW_SUCCESS;
    
    dm_net_ap_t *ap = &g_aps[g_ap_count];
    // 使用 memcpy 防止 SSID 越界溢出
    memcpy(ap->ssid, r->ap_details.SSID.val, r->ap_details.SSID.len);
    ap->ssid[r->ap_details.SSID.len] = '\0';
    ap->security = r->ap_details.security;
    ap->rssi = r->ap_details.signal_strength;
    g_ap_count++;
    return RTW_SUCCESS;
}

// [触发层] 每次进入页面或点击刷新时调用
int dm_net_wifi_scan_start(void) {
    g_ap_count = 0;
    g_scan_done = 0;
    return wifi_scan_networks(safe_scan_handler, NULL); // user_data 必须为 NULL
}
```

### 2. WiFi 后台阻塞连接与脱逃范式 (`dm_net.c`)
```c
static int current_conn_gen = 0; // 连接代数

static void *safe_wifi_conn_worker(void *arg) {
    int my_gen = current_conn_gen;
    char *ssid = (char *)arg;
    int ret;
    
    if (strlen(g_psk_buf) > 0) {
        // WPA2 校验：拦截少于 8 字节的非法下发
        if (strlen(g_psk_buf) < 8) ret = RTW_INVALID_KEY;
        else ret = wifi_connect(ssid, RTW_SECURITY_WPA2_AES_PSK, g_psk_buf, strlen(ssid), strlen(g_psk_buf), 0, NULL);
    } else {
        // OPEN 直连：严防传入非 NULL
        ret = wifi_connect(ssid, RTW_SECURITY_OPEN, NULL, strlen(ssid), 0, 0, NULL);
    }
    
    // 脱逃判断：如果代数变了说明 UI 已经放弃了这次连接
    if (my_gen == current_conn_gen) {
        g_conn_state = (ret == RTW_SUCCESS) ? 2 : 3;
        if (ret == RTW_SUCCESS) {
            atomic_write_wapi_conf(ssid, g_psk_buf); // 调用原子写入
        }
    }
    free(ssid);
    return NULL;
}
```

### 3. Bluetooth 安全回调注册与状态轮询范式 (`dm_net.c`)
```c
// 蓝牙静态缓存池
static bt_discovery_result_t g_bt_devs[DM_NET_BT_MAX];
static int g_bt_dev_count = 0;

static void on_bt_discovery_result_cb(bt_discovery_result_t* remote) {
    if (!remote) return;
    // MAC 地址去重逻辑
    for (int i = 0; i < g_bt_dev_count; i++) {
        if (memcmp(&g_bt_devs[i].address, &remote->address, sizeof(bt_address_t)) == 0) return;
    }
    if (g_bt_dev_count < DM_NET_BT_MAX) {
        memcpy(&g_bt_devs[g_bt_dev_count++], remote, sizeof(bt_discovery_result_t));
    }
}

// UI 点击启动扫描
void dm_net_bt_start_scan(void) {
    if (!g_bt_ins) return; // 空指针防御
    if (bt_adapter_get_state(g_bt_ins) != BT_ADAPTER_STATE_ON) return; // 状态抢跑防御
    
    g_bt_dev_count = 0;
    adapter_callbacks_t cbs = {0};
    cbs.on_discovery_result = on_bt_discovery_result_cb;
    // TODO: 绑定其他必要回调如 on_pair_request 等
    bt_adapter_register_callback(g_bt_ins, &cbs);
    
    bt_adapter_start_discovery(g_bt_ins, 15); // 15秒超时自动结束
}
```

---

## 七、 🎨 基于现有框架的 UI 草图与样式复用规范 (UI Draft & Style Reuse) 🎨

在梳理完 `apps/luncher_dm/deskmate_ui.c` 的源码后，发现当前使用了纯粹的 iOS-style 扁平大圆角卡片风。为了保持视觉的高度统一，WiFi 和 蓝牙的子页面**必须严格复用**以下辅助构建函数，禁止私自魔改长宽与颜色。

### 1. 样式库对齐规范
- **排版容器**：必须使用 `LV_FLEX_FLOW_COLUMN` 配合 `lv_obj_set_style_pad_row(parent, 4, 0)`。
- **分组卡片**：使用 `settings_group_card(parent)` 统一创建带有阴影和 `RAD_CARD` 圆角的纯白背景卡片。
- **组标题 (小字)**：使用 `settings_section_label(parent, "MY NETWORKS")` 来做卡片上方的灰色标题。
- **选项行 (Row)**：使用带箭头指示的 `settings_row_value()`，或者带图标的自定义 Clean Cont，并在每个 Row 之间穿插 `settings_add_separator(card)` 灰色分割线。

### 2. WiFi 子页实现草图 (Pseudo UI Code)

```c
static void create_wifi_subpage(lv_obj_t *parent) {
    /* 1. 复用大标题 */
    subpage_big_title(parent, "WiFi");
    
    /* 2. 当前网络组 */
    settings_section_label(parent, "CURRENT NETWORK");
    lv_obj_t *cur_card = settings_group_card(parent);
    
    // 【已连接行】使用 value 行或自定义 click 行
    // e.g. "TP-LINK_5G"          [绿勾] >
    lv_obj_t *cur_row = settings_row_value(cur_card, LV_SYMBOL_WIFI, COL_GREEN, "TP-LINK_5G", "Connected", true);
    lv_obj_add_event_cb(cur_row, wifi_current_click_cb, LV_EVENT_CLICKED, NULL);
    
    /* 3. 附近网络组 */
    settings_section_label(parent, "OTHER NETWORKS");
    lv_obj_t *list_card = settings_group_card(parent);
    
    // 此处预留为空卡片，后续由 wifi_poll_cb 轮询到底层结果后，
    // 在 list_card 里面循环往复创建 row 并使用 settings_add_separator() 隔开。
    wifi_list_cont = list_card; 
    
    /* 4. 挂载生命周期 Timer */
    wifi_timer = lv_timer_create(wifi_poll_cb, 500, NULL);
    // 强制绑定析构：父容器销毁时销毁 timer，防野指针
    lv_obj_add_event_cb(parent, wifi_page_destructor_cb, LV_EVENT_DELETE, NULL);
}

// 析构防护回调
static void wifi_page_destructor_cb(lv_event_t *e) {
    if (wifi_timer) {
        lv_timer_del(wifi_timer);
        wifi_timer = NULL;
    }
}
```

### 3. Bluetooth 子页实现草图 (Pseudo UI Code)

```c
static void create_bt_subpage(lv_obj_t *parent) {
    subpage_big_title(parent, "Bluetooth");
    
    /* 1. 控制组 */
    lv_obj_t *ctrl_card = settings_group_card(parent);
    
    // 蓝牙主开关
    settings_row_switch_cb(ctrl_card, LV_SYMBOL_BLUETOOTH, COL_BLUE, "Bluetooth", dm_net_bt_is_on(), bt_main_switch_cb);
    settings_add_separator(ctrl_card);
    // 可发现模式开关 (Discoverable)
    settings_row_switch_cb(ctrl_card, LV_SYMBOL_EYE_OPEN, COL_ORANGE, "Discoverable", false, bt_discover_switch_cb);

    /* 2. 已配对设备组 */
    settings_section_label(parent, "MY DEVICES");
    lv_obj_t *paired_card = settings_group_card(parent);
    // 根据配对列表填充: 
    // settings_row_value(paired_card, LV_SYMBOL_BLUETOOTH, COL_SEC, "AirPods Pro", "Not Connected", true);
    
    /* 3. 附近设备组 */
    settings_section_label(parent, "OTHER DEVICES");
    lv_obj_t *other_card = settings_group_card(parent);
    bt_other_list_cont = other_card; // 留给轮询更新
    
    /* 4. 生命周期管理 */
    bt_timer = lv_timer_create(bt_poll_cb, 500, NULL);
    lv_obj_add_event_cb(parent, bt_page_destructor_cb, LV_EVENT_DELETE, NULL);
}
```

---

## 八、 💣 deskmate_ui.c 源码 BUG 扫雷报告 (必修课) 💣

> 我已对 `apps/luncher_dm/deskmate_ui.c` 进行了代码审查，抓出了以下几个由于“粗心大意”留下的毁灭性 BUG。AI 在编写代码时必须引以为戒，主动修复！

1. **致命漏洞 1：孤儿键盘界面 (Orphaned Overlay)**
   - **坐标**：`wifi_ap_click_cb()` 中的 `wifi_pwd_overlay = lv_obj_create(scr);`
   - **病症**：把密码输入遮罩层直接挂载到了根屏幕 `scr` 上，而不是当前子页的 `subpage_overlay` 内。这意味着，如果用户点开输入密码界面，然后直接点击系统的**物理返回键 (关闭子页)**，`close_subpage()` 只会删掉 `subpage_overlay`。此时，`wifi_pwd_overlay` 这个黑色的键盘层就变成了孤儿，永远留在了屏幕上遮挡一切。
   - **修复要求**：遮罩层绝不能乱挂，必须挂在能被销毁的生命周期树上，或者在 `close_subpage` 里显式调用 `lv_obj_del(wifi_pwd_overlay)`。

2. **致命漏洞 2：字符串复制缺失截断 (Missing Null-Termination)**
   - **坐标**：`strncpy(wifi_pwd_ssid, ap.ssid, sizeof(wifi_pwd_ssid) - 1);`
   - **病症**：用 `strncpy` 拷贝完长度 32 的 SSID 之后，**没有补 `\0`**。如果遇到的 AP 名称恰好是满 32 个字符，下一行 `lv_label_set_text_fmt(ttl, "连接 %s", wifi_pwd_ssid)` 就会把内存里后面不可控的脏数据全打印出来，大概率直接内存越界死机。
   - **修复要求**：永远加上 `wifi_pwd_ssid[32] = '\0';`。

3. **致命漏洞 3：多重进入导致的内存泄漏与指针覆盖 (Multiple Entrance Leak)**
   - **坐标**：`show_subpage` 和 `create_wifi_subpage`
   - **病症**：代码没有判断 `subpage_overlay` 是否已存在就直接 `lv_obj_create`。如果因为点击事件抖动，瞬间调了两次 `show_subpage("WiFi")`，就会创建两层 UI，且 `subpage_overlay` 和 `wifi_timer` 的指针直接被覆盖。你再调用 `close_subpage()` 时，只能删掉最后一次的界面，前一次的定时器和界面永远驻留内存。
   - **修复要求**：任何单例对象的创建前，必须执行判空 `if(subpage_overlay) return;` 防抖保护。
