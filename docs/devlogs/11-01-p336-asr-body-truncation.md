# P336 — 小米 ASR 请求体被截断（400 Invalid JSON）

**结论**：`xiaomi_asr.c` 的 `build_body()` 前缀长度算错，导致 base64 请求体在
`data:audio/wav;base64,` 处就结束，MiMo 返回 400 `Invalid JSON in request body`。
**已修，板端验证通过**（2026-11-01 16:41 实机 `ASR: 今天多少`）。

## 现象

板端每次录音后固定报：

```
[llm] ... [xiaomi_asr] HTTP 400: {"error":{"code":"InvalidJson",...}}
```

## 根因

```c
/* 旧 */
prefix_len = strlen(prefix) + strlen(s_model);   /* prefix 里含 "%s" 两个字节 */
```

`prefix` 是 `"...{\"model\":\"%s\",...}"` 这类格式串，`strlen` 把占位符
`%s` 的 2 个字节也算进去了；`snprintf` 实际只写入 model 的字符数。结果
`prefix_len` 比真实前缀长 2 字节 → base64 起点后移 2 字节 → 写进去的是
`snprintf` 的 `\0` 和一个未初始化字节 → `vela_https_post_json()` 用
`strlen()` 算长度，body 到 `base64,` 就结束。

```c
/* 新 */
prefix_len = strlen(prefix) - 2 + strlen(s_model);
```

同时加了 `snprintf` 返回值检查（返回 ≥ 缓冲长度说明截断，直接失败而不是发半截）。

## 证据

宿主复现 `/tmp/asr_body_test.c`：旧算法产出 130 字节非法 JSON，新算法 42897 字节合法。
板端修后 ASR 正常出字。

## 教训

凡是"用格式串长度估算输出长度"，先减掉占位符字节数；更稳的是直接用
`snprintf` 的返回值。
