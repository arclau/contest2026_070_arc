/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_health_cfg.h
 * DesktopMate — 配置文件（/data/dm_health.cfg，yaffs 可写、重启保留）
 *
 * 架构：所有「以后可能要改」的文案与参数入配置，硬编码默认值兜底。
 *  - 文件缺失/损坏/缺项 → 回退编译期默认值（不崩）
 *  - 首次启动自动生成带注释的默认模板，方便直接编辑
 *  - 第②层「语音改参数」通过 dm_health_cfg_set_* 运行时落盘
 *
 * 设计定稿：project_docs/specs/health-assistant-design.md
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
 ****************************************************************************/

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_HEALTH_CFG_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_HEALTH_CFG_H

/* 配置路径：/data（yaffs，可写、重启保留，仿 dm_music.state） */
#define DM_HEALTH_CFG_FILE "/data/dm_health.cfg"

/* 启动时加载（dm_health_init 调用）：读文件 → 缺项回退默认；
 * 文件不存在则先写默认模板再回退默认值。 */
void dm_health_cfg_load(void);

/* 读取：key 不存在/非法时返回传入的默认值（调用方给硬编码兜底） */
int         dm_health_cfg_get_int(const char *key, int def);
const char *dm_health_cfg_get_str(const char *key, const char *def);

/* 运行时修改并落盘（第②层语音改参数用）；返回 0 成功 / -1 失败 */
int dm_health_cfg_set_int(const char *key, int value);
int dm_health_cfg_set_str(const char *key, const char *value);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_HEALTH_CFG_H */
