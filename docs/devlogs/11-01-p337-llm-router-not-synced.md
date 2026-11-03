# P337 — 扫码写的 MiMo Key 到不了 LLM（router 没同步）

**结论**：设置页扫码把 Key 写进 `llm_proxy` 全局量，但每轮对话前
`agent_loop` 会 `llm_router_apply(0)` 用 router 自己的 backend 覆盖回去。
router slot 0 仍是构建期默认（火山 Ark + `deepseek-v4-flash-260425`），
于是 ASR/TTS 正常、LLM 报 400 `InvalidEndpoint.ClosedEndpoint`。
**已修（pair_server 同步 router slot 0），镜像 `cf017943` 待刷。**

## 现象

```
ASR: 今天多少
[llm] API error 400: {"error":{"code":"InvalidEndpoint.ClosedEndpoint",
  "message":"The request targeted an endpoint that is currently closed or
  temporarily unavailable. Request id: 0217935224853374..."}}
[agent] LLM call failed on backend 0, trying failover
```

助手最终回"出错"。

## 定位过程

1. ASR 走通了 → Key 本身有效，问题只在 LLM 这一路。
2. 错误 JSON 的 `Request id` 是 `021...` + 48 位十六进制，是**火山引擎**的
   请求 ID 格式，不是 MiMo 的 → 说明请求根本没发到 `api.xiaomimimo.com`。
3. 读码确认：`agent_loop.c:1102` `llm_router_select()` → `llm_router_apply(idx)`
   → `llm_set_all(router 里那份 host/path/key/model)`。`pair_server.c` 只调了
   `llm_set_all(...)`，**没有写 router**；router 初始化时无持久 backend 就回落到
   `agent_secrets.h` 的 Ark Key + `deepseek-v4-flash-260425`。
4. 那个模型在该 Ark 账号上已关闭 → `ClosedEndpoint`。

## 修法

`pair_server.c` `/api/config` POST 保存后，除 `llm_set_all()` 外再写一次
router slot 0（host/path/port/model/key 全用 MiMo 的）并 `llm_router_apply(0)`，
让"扫码一个 Key 打通 LLM+ASR+TTS"名副其实。

同时把 `llm_proxy.c` 的 400 日志加上 `host=` 和 `model=`：

```
[llm] API error 400 (host=api.xiaomimimo.com model=mimo-v2.6-flash): ...
```

板端 console 只放 WARNING 及以上，原来那条 `OpenAI API with tools (model: ...)`
是 LOG_INFO，看不到——所以下次再出现同类问题，一条日志就能分辨是谁在报错。

## 待验

刷 `cf017943` → 重新扫码填 Key → 说一句话，确认：
1. `ASR: ...` 有字；
2. 无 `API error`；
3. 助手正常回答并出声。
