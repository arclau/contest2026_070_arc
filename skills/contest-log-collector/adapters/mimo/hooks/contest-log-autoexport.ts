// SPDX-License-Identifier: Apache-2.0
// contest-log-autoexport — 会话结束自动写入本地私人仓 logs/
// 触发: session.post（runLoop 结束，含 error/cancel）
// 行为: 调 export_mimo_db.py 把当前 session 导出到
//       /data/dm/contest2026_070_arc/logs/<login>/<date>/
// 纪律: 只写文件，不 git commit/push（上传由选手控制）

import { spawn } from "child_process";
import { existsSync } from "fs";

const EXPORT_PY =
  "/data/dm/.agents/skills/contest-log-collector/scripts/export_mimo_db.py";
const REPO = "/data/dm/contest2026_070_arc";
const SKIP_AGENTS = new Set([
  "checkpoint-writer",
  "title",
  "summary",
  "compaction",
]);

function runExport(sessionID: string): Promise<number> {
  if (!existsSync(EXPORT_PY)) {
    console.error(`[contest-log] export script missing: ${EXPORT_PY}`);
    return Promise.resolve(1);
  }
  return new Promise((resolve) => {
    const child = spawn(
      "python3",
      [EXPORT_PY, "--session", sessionID, "--repo", REPO],
      { stdio: ["ignore", "pipe", "pipe"] },
    );
    let out = "";
    let err = "";
    child.stdout.on("data", (d) => (out += String(d)));
    child.stderr.on("data", (d) => (err += String(d)));
    child.on("close", (code) => {
      if (code === 0) {
        console.error(`[contest-log] autoexport ok: ${out.trim()}`);
      } else {
        console.error(
          `[contest-log] autoexport failed code=${code} out=${out.trim()} err=${err.trim()}`,
        );
      }
      resolve(code ?? 1);
    });
    child.on("error", (e) => {
      console.error(`[contest-log] autoexport spawn error: ${e}`);
      resolve(1);
    });
  });
}

export default {
  "session.post": async (input: {
    sessionID: string;
    agentID: string;
    task_id?: string;
    outcome: string;
  }) => {
    // 只导出主会话，跳过内部 subagent / checkpoint-writer
    if (SKIP_AGENTS.has(input.agentID)) return;
    if (input.task_id) return;
    if (!input.sessionID?.startsWith("ses_")) return;
    // await 写盘完成，避免 runLoop 退出竞态
    await runExport(input.sessionID);
  },
};
