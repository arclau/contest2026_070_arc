# P361 天气地点由惠州龙门改到北京（卡片 + AI 语音）

目标：首页天气卡和 AI 问天气都从"惠州龙门"改到"北京"。
平台：V1 `ynm3000` / `luncher_dm` + `ai_agent`。

## 一、两个地点来源

1. **首页天气卡**：`dm_city.c` 的 12 城表 + `DM_CITY_DEFAULT_IDX`，坐标进
   open-meteo 请求；Settings 里可循环切换，序号落盘 `weather_city_idx`。
2. **AI 语音天气**：`ai_agent` 不自己查天气，读 `luncher_dm` 落的
   `/data/dm_weather.state`（`温度 湿度 描述 城市`），城市字段来自 `dm_city_cur()`。
   另有 `USER.md` / `MEMORY.md` 里的"位置/天气关注城市"作为人设上下文，
   LLM 会拿它兜底，也必须一起改。

## 二、改动

- `dm_city.h`：`DM_CITY_DEFAULT_IDX` 0 → 3（表里本来就有北京 39.90/116.40）。
  表不动——序号是落盘键，改顺序会打乱已有设置。
- `dm_city.c`：`dm_city_cur_idx()` 加一次性迁移：已落盘的 0（旧默认惠州）
  迁到北京并写标记 `weather_city_idx_moved`；有标记后手动选回惠州不会被改回去。
- `dm_weather.c`：删掉没人用的 `DM_W_LAT/DM_W_LON` 写死坐标（惠州），
  坐标只在 dm_city 表维护一份。
- `memory_store.c`：`USER.md` 默认文案改"位置：北京市 / 天气关注城市：北京"；
  `migrate_legacy_name` 泛化成 `migrate_replace(path, old, new)`，一并处理
  老文件里的"惠州龙门"。顺手修掉不等长替换的坑——原来就地 `memcpy` 只在
  等长时安全，"惠州龙门"→"北京" 会留下尾巴变成"北京龙门"，改成先拷到输出缓冲。
- `agent_skills/templates/USER.md`：同步。
- `ui_settings.c`：只改注释（默认城市名）。

## 三、验证

`pack_deskmate.sh ynm3000` PASS（config / entrypoint / startup / 121 resources /
nsh / whole image），日志里 `memory_store.c`、`dm_city.c`、`dm_weather.c`、
`ui_settings.c` 都重编了；`vela.bin == nsh.fex` 一致。
镜像 `output/ynm3000/ynm3000.img` 53,149,696 B，MD5 `762fa907796db7e35a413cd6530a19f2`。

待验（板上）：① 天气卡显示北京的温度（与手机北京天气对得上）；② 问"今天天气
怎么样"，维拉说的是北京；③ Settings 城市行显示"北京"；④ 点一下能循环切到别的
城市并立即刷新。
