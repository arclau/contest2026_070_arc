# 9-09 part3 devlog — P206 交作业专项启动：SDK清理 + deskmate配置 + 专属仓070

> 日期：2026-09-09 ｜ 节点：P206 ⏳ ｜ 固化：ok-20260909-8（deskmate配置新建）
> 赛规源：`project_docs/submission/official/{contest_overview,code_submission_guide,ai_coding_log_guide}.md` + 作品提交模板docx（已解析）

## 一、SDK清理（git vs 本地比对）

- 全仓 repo结构：顶层非git，249个project；`git_snapshot.sh`只固化三仓（vendor/allwinnertech+nuttx+apps），三仓status干净。
- 删：`_trash`隔离→`rm -rf`共1.9G（flymeicons原料1.2G+两APK 736M+TTS zip+猫单图/脚本/日志）+ 中文wav 4个（2.5M，md5与res/tones英文版逐字节一致）+ 宠物三件套（catimg 5M/tools/cat_sprites空）+ laojie.mp3。
- 用户手动删：`flyme_icons/`成品1.1G（用不上）。
- 归档未删：AIYOUHUA/DOUBAO/uiyouhua/voicedesight/交作业→`project_docs/archive/origin-docs-20260909/`；LD2410B两PDF保留→后移`project_docs/datasheets/`。
- project_docs分类：specs→pet/voice/wireless/misc四组；VOICEDESIGN.md→specs/voice/；AGENTS第5行+README目录表同步。

## 二、deskmate提交配置（ok-20260909-8）

- 新建`boards/r528/r528s3-gemini-s1/configs/deskmate/defconfig`（=nsh逐字节拷贝起步）；`nsh/`保持上游干净。
- 新配置编译→打包→check_res✅（23提示音+字体+res.fex）→snapshot固化；lunch菜单已认`m deskmate`。
- AGENTS §一/§二两处nsh→deskmate；子devlog历史nsh引用故意不动。

## 三、公共仓改动定性

- `apps/graphics/lvgl/lvgl` 3文件（GE2D gating/触摸物理分辨率clamp/缺字形占位）：通用bugfix→走公共仓PR到`dev-ai-contest-2026`，不搬私仓。
- `tests/Kconfig`（$APPSDIR→绝对路径）=排障手误→已`git checkout`还原。

## 四、专属仓contest2026_070_arc（liujinye-sys邀请）

- 已clone看结构：app/quickapp/board模板+`contest2026_070_arc.xml`（linkfile映射）+logs/README+CLA检查。
- 映射方案（待执行）：`r528/luncher_dm→vendor/allwinnertech/apps/luncher_dm`；`r528/deskmate→configs/deskmate`；`r528/res_tones→res/tones新增`；删三模板；改README；装日志采集+backfill。
- 赛道：AI硬件产品创新（单项）→报告3.4须写运行时Skill（/data/agent/skills/，当前缺，待办）。
- 截止9.20剩11天；待用户：Accept邀请→fork→给GitHub用户名。

## 五、隐私模式策略（方案先行，代码回退，本会话定案）

> ⚠️ 全流程基线（重要）：全局代码未稳定，**先恢复原样、方案落文档**；隐私模式=提交前最后一次编译前的`ok-20260909-10`基线，按`提交检查清单.md §〇/§三`一键执行。

**敏感点定位**：豆包/火山全家桶凭证不在`luncher_dm`，在`packages/ai_agent/include/agent_secrets.h`（ark-…方舟key + 豆包APPID <REDACTED-APPID> + token + cluster），被`volc_tts.c/volc_asr.c/llm_router.c`引用；`git status`确认**untracked从未提交**，`agent_config.h`用`__has_include`外挂加载=上游零污染。

**对标他队**（open-vela实战）：106 VelaGoGoGo 同款豆包方案=`agent_secrets.h.example`模板进仓+真文件`.gitignore`忽略+README写"火山控制台拿APP ID/Token填"；062纯硬件无此问题。**行业标准答案=模板+忽略+README教填**，无队做二维码服务端。

**今日本会话实际操作**：
- 新建`contest2026_070_arc/r528/agent_secrets.h.example`（key全部`YOUR_*_HERE`占位）+ fork根`.gitignore`（忽略`**/agent_secrets.h`/`*.env`/`config.json`）——搬文件时同步提交。
- 临时探测（未保留）：`主人→主人`改2行(ui_home.c:1129/dm_health_cfg.c:75)、`checkout 3fd20d5b`还原wapi.conf，随后**全部回退**，代码回到原有基线（"主人"+手机号版wapi.conf）。
- 途中误把"主人"版固化进ok-20260909-9 → 已用`checkout 71bf86ec(--ok-9-7)`还原"主人" → 固化 **ok-20260909-10** = 当前隐私策略基线。
- 结论/报告写入`project_docs/submission/提交检查清单.md`（§〇策略表+§三动手顺序，最后一行参考）。

**待办（隐私模式，push前执行）**：改"主人→主人"2行 + `checkout 3fd20d5b`还原wapi.conf(手机号) + 搬fork带`.gitignore/agent_secrets.h.example` + 板端填key UI(Phase 2可选) + 报告写清评委自填。

## 六、fork搬家（本地完成，未commit未push，9-09晚）

- 搬：`r528/luncher_dm`（64M，已剔350个.o/107M+Make.dep）+ `r528/deskmate/defconfig` + `r528/res_tones`（24 wav/4.6M）。
- xml：linkfile改三条r528映射，删app/quickapp/board三模板骨架。
- README：重写为作品说明版（含凭证填写/编译/打包/硬件清单/官方链接）。
- skills/×3：dts-to-vela-mipi（1005行）+deskmate-ui（787行）+product-designer-ui（297行）；openvela-architect因 Treo 320×240过时已删。
- `.gitignore`：启用模板+追加`**/agent_secrets.h`/`*.env`/`config.json`；logs/保持可提交。
- 下一步：fork内commit→PR自行合入→装日志采集+backfill→lvgl公共PR→CLA→报告→视频。

## 待办（下会话）

1. fork后搬文件+补xml/README+装采集器+backfill+PR+CLA。
2. 技术报告（模板§二1~3.7）+演示视频≤5min+`<队>-<作品>-<仓>.zip`。
3. P200~P205上板验证仍挂起（与交作业并行）。
4. 记得此会话已建`提交检查清单.md`（含隐私策略§〇）+ 基线**ok-20260909-10**，勿重复探测。.
