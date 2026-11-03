/****************************************************************************
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#include <nuttx/config.h>
#ifdef CONFIG_GEMINI_S1_NSH

#if !(defined(CONFIG_BOARDCTL_RESET_CAUSE) && !defined(CONFIG_NSH_DISABLE_RESET_CAUSE))
#error "Deps: defined(CONFIG_BOARDCTL_RESET_CAUSE) && !defined(CONFIG_NSH_DISABLE_RESET_CAUSE)"
#endif
#if !defined(CONFIG_KVDB)
#error "Deps: defined(CONFIG_KVDB)"
#endif

set -x
echo "You're running an nsh image."

mount -t romfs /dev/res /resource
mount -t yaffs /dev/usrdata /data
set errcode $?
if [ $errcode -ne 0 ]
then
  echo "/dev/usrdata mount failed, errcode is" $errcode
fi

#ifdef CONFIG_SYSTEM_ADBD
adbd &
#endif

#ifdef CONFIG_KVDB_SERVER
kvdbd &
#endif

set resetcause `resetcause`
echo $resetcause

if [ "$resetcause" == "cpu_soft_reset(restore)" ]
then
  echo "recovery reset system"
  . /etc/factory.sh
  echo "reboot system now"
  reboot
fi

#ifndef CONFIG_LCD_DEV
#ifdef CONFIG_SHOW_LOGO
/* Do not show logo if system crashed or during silient OTA. */
if [ "$resetcause" == "cpu_soft_reset(panic)" ]
then
  echo "System crashed, hide logo"
else
  set silent_reboot_flag `getprop persist.silent_reboot.flag`

  /* Show logo firstly before reset system, if rst_btn is pressed.*/
  if [ -z $silent_reboot_flag -o $silent_reboot_flag == false ]
  then
    showlogo -n /resource/logo/logo1.bin
  fi
fi
#endif
#endif

#ifdef CONFIG_SYSTEM_NTPC
ntpcstart &
#endif

/* 2026-08-15 启动优化 v2：UI/蓝牙不依赖 WiFi，提前启动（省 ~20s 串行等待）。
 * luncher_dm 纯 UI（天气/AI 子页联网后自刷新），蓝牙走 H4 UART 与 WiFi SDIO 独立。
 * ai_agent 保留在 WiFi 固件初始化后启动（P70 教训：与 WiFi 固件下载并发会 0x27）。
 * 0x27 风险点 = ai_agent vs WiFi 固件下载；UI/蓝牙不碰 WiFi 固件，提前无风险。 */
#ifdef CONFIG_LUNCHER_DM_V2_APP
luncher_dm_v2 &
#elif defined(CONFIG_LUNCHER_DM_APP)
luncher_dm &
#elif defined(CONFIG_LUNCHER_APP)
luncher &
#endif

/* TIANMA 1080x2160 点屏实验: NSH 镜像开机直接跑 LVGL widgets demo。
 * deskmate 各配置未开 CONFIG_EXAMPLES_LVGLDEMO，互不影响。 */
#ifdef CONFIG_EXAMPLES_LVGLDEMO
lvgldemo widgets &
#endif

/* 2026-09-15 暂时关闭蓝牙服务（用户要求，勿删；配合 r528_boot.c 注释掉
 * rtk8723FS_initialize——bt_recv 内核线程空转占满 CPU1，且蓝牙 H5 一直
 * bthci open timeout(-110) 不可用）。以后要用再放开。 */
#ifdef CONFIG_BLUETOOTH_SERVER
/* echo "Starting bluetoothd..." */
/* bluetoothd & */
#endif

if [ -f /data/etc/wifi/wapi.conf ]
then
  /* 2026-08-12 用户指示：AI 启动延后——start_wifi.sh 前台执行（去 &），
   * 等 WiFi 初始化（固件下载/连接/DHCP）完成后再启动 ai_agent，
   * 消除 ai_agent 提前启动与 WiFi 驱动初始化的并发干扰（download_fw 0x27）。 */
  . /etc/wifi/start_wifi.sh
fi

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA
/* 2026-08-11 P53: AI Agent 开机自启——LLM Key/模型/语音凭证均已编译注入
 * agent_secrets.h（deepseek-v4-flash-260425 + 豆包语音），无需 set_* 命令，
 * 启动即用。 */
/* 2026-08-11 P59: stdin 重定向 /dev/null —— ai_agent 的 cli_thread 会
 * fgets(stdin) 阻塞抢 NSH 控制台（vela> 提示符占住串口，回不到 nsh）。
 * 重定向后 CLI 线程读 EOF 立即退出，控制台归 NSH；AI 交互走 WS/UI。 */
echo "Starting ai_agent..."
ai_agent </dev/null &
#endif

echo "Boot nsh ok"
#endif /* CONFIG_GEMINI_S1_NSH */
