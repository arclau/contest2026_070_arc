############################################################################
# apps/examples/helloxx/Makefile
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################
include $(APPDIR)/Make.defs
include $(APPDIR)/vendor/allwinnertech/Common.mk

MAINSRC = luncher_dm.c
# Phase 1 拆分（2026-08-09）：子页与主界面已从 deskmate_ui.c 迁至 ui/
CSRCS = aibg.c pet_frame_table.c lv_demo_panel_rgb_control.c deskmate_ui.c dm_weather.c deskmate_icons.c dm_asan_stub.c dm_net.c dm_ai.c dm_health.c dm_health_cfg.c dm_voice.c dm_ld2410b.c dm_uart_dbg.c dm_als.c dm_pet.c ui/ui_wifi.c ui/ui_bt.c ui/ui_books.c ui/ui_files.c ui/ui_settings.c ui/ui_music.c ui/ui_home.c ui/ui_uart_dbg.c pet_cat_idle_00.c pet_cat_idle_01.c pet_cat_idle_02.c pet_cat_idle_03.c pet_cat_idle_04.c pet_cat_idle_05.c pet_cat_idle_06.c pet_cat_idle_07.c pet_cat_idle_08.c pet_cat_idle_09.c pet_cat_idle_10.c pet_cat_idle_11.c pet_cat_walk_00.c pet_cat_walk_01.c pet_cat_walk_02.c pet_cat_walk_03.c pet_cat_walk_04.c pet_cat_walk_05.c pet_cat_walk_06.c pet_cat_walk_07.c pet_cat_walk_08.c pet_cat_walk_09.c pet_cat_walk_10.c pet_cat_walk_11.c pet_cat_groom_00.c pet_cat_groom_01.c pet_cat_groom_02.c pet_cat_groom_03.c pet_cat_groom_04.c pet_cat_groom_05.c pet_cat_groom_06.c pet_cat_groom_07.c pet_cat_groom_08.c pet_cat_groom_09.c pet_cat_groom_10.c pet_cat_groom_11.c pet_cat_sleep_00.c pet_cat_sleep_01.c pet_cat_sleep_02.c pet_cat_sleep_03.c pet_cat_sleep_04.c pet_cat_sleep_05.c pet_cat_sleep_06.c pet_cat_sleep_07.c pet_cat_sleep_08.c pet_cat_sleep_09.c pet_cat_sleep_10.c pet_cat_sleep_11.c pet_cat_eat_00.c pet_cat_eat_01.c pet_cat_eat_02.c pet_cat_eat_03.c pet_cat_eat_04.c pet_cat_eat_05.c pet_cat_eat_06.c pet_cat_eat_07.c pet_cat_eat_08.c pet_cat_eat_09.c pet_cat_eat_10.c pet_cat_eat_11.c pet_cat_play_00.c pet_cat_play_01.c pet_cat_play_02.c pet_cat_play_03.c pet_cat_play_04.c pet_cat_play_05.c pet_cat_play_06.c pet_cat_play_07.c pet_cat_play_08.c pet_cat_play_09.c pet_cat_play_10.c pet_cat_play_11.c pet_cat_happy_00.c pet_cat_happy_01.c pet_cat_happy_02.c pet_cat_happy_03.c pet_cat_happy_04.c pet_cat_happy_05.c pet_cat_happy_06.c pet_cat_happy_07.c pet_cat_happy_08.c pet_cat_happy_09.c pet_cat_happy_10.c pet_cat_happy_11.c pet_cat_sad_00.c pet_cat_sad_01.c pet_cat_sad_02.c pet_cat_sad_03.c pet_cat_sad_04.c pet_cat_sad_05.c pet_cat_sad_06.c pet_cat_sad_07.c pet_cat_sad_08.c pet_cat_sad_09.c pet_cat_sad_10.c pet_cat_sad_11.c pet_cat_hungry_00.c pet_cat_hungry_01.c pet_cat_hungry_02.c pet_cat_hungry_03.c pet_cat_hungry_04.c pet_cat_hungry_05.c pet_cat_hungry_06.c pet_cat_hungry_07.c pet_cat_hungry_08.c pet_cat_hungry_09.c pet_cat_hungry_10.c pet_cat_hungry_11.c pet_cat_curious_00.c pet_cat_curious_01.c pet_cat_curious_02.c pet_cat_curious_03.c pet_cat_curious_04.c pet_cat_curious_05.c pet_cat_curious_06.c pet_cat_curious_07.c pet_cat_curious_08.c pet_cat_curious_09.c pet_cat_curious_10.c pet_cat_curious_11.c pet_cat_celebrate_00.c pet_cat_celebrate_01.c pet_cat_celebrate_02.c pet_cat_celebrate_03.c pet_cat_celebrate_04.c pet_cat_celebrate_05.c pet_cat_celebrate_06.c pet_cat_celebrate_07.c pet_cat_celebrate_08.c pet_cat_celebrate_09.c pet_cat_celebrate_10.c pet_cat_celebrate_11.c pet_cat_touch_00.c pet_cat_touch_01.c pet_cat_touch_02.c pet_cat_touch_03.c pet_cat_touch_04.c pet_cat_touch_05.c pet_cat_touch_06.c pet_cat_touch_07.c pet_cat_touch_08.c pet_cat_touch_09.c pet_cat_touch_10.c pet_cat_touch_11.c

# P201 pet 三层拆分：core=引擎，view=视图，dm_pet.c=门面（gen 脚本只管 pet_cat_*，勿动此行）
CSRCS += pet_core.c pet_view.c

# 2026-09-10 城市设置（A 方案：天气城市级定位）
CSRCS += dm_city.c ui/ui_pet.c ui/ui_calendar.c dm_alarm.c ui/ui_alarm.c

# 2026-09-10 P211：日期与时间独立子页（Settings 嵌套，宠物同款）
CSRCS += ui/ui_datetime.c

# 2026-09-11 截图（LRADC 按键触发全屏 BMP，本地固件）
CSRCS += dm_screenshot.c

# mcpserver built-in application info

PROGNAME = $(CONFIG_LUNCHER_DM_APP_PROGNAME)
PRIORITY = $(CONFIG_LUNCHER_DM_APP_PRIORITY)
STACKSIZE = $(CONFIG_LUNCHER_DM_APP_STACKSIZE)
MODULE = $(CONFIG_LUNCHER_DM_APP)

# Use common HAL includes
$(eval $(AW_ADD_HAL_INCLUDES))

# Phase 1 拆分：ui/ 子目录源文件需包含本目录头（deskmate_ui.h / dm_net.h）
CFLAGS += ${INCDIR_PREFIX}$(CURDIR)

# 2026-09-11 截图 PNG：libpng/zlib 已编入 libapps.a，这里只需头文件路径
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/libpng/libpng
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/system/zlib/zlib

# WiFi (RTL8733BS) — status query + on/off from UI (wifi_conf.h)
$(eval $(AW_ADD_WIFI_INCLUDES))

# Bluetooth framework (ZBlue) — bt_adapter API, socket IPC to bluetoothd
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/frameworks/connectivity/bluetooth/framework/include

# libcedarx (XPlayer) + aw-alsa-lib headers for the music playback backend
# (mirror of libcedarx/Make.defs include list)
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/libcore/include
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/libcore/base/include
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/libcore/common/iniparser
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/libcore/common/plugin
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/libcore/stream/include
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/libcore/parser/base/id3base
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/libcore/parser/include
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/libcore/parser/flac
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/libcore/parser/pls
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/libcore/playback/include
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/xplayer/include
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/decoding/osal
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/decoding/include
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/decoding/tools/audiomix/src
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/external/include/adecoder
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/external/include/libcedarc
CFLAGS += ${INCDIR_PREFIX}$(AW_CHIP_DIR)/components/multimedia/src/libcedarx/external/include/zlib
CFLAGS += ${INCDIR_PREFIX}$(AW_HAL_DIR)/include/hal/aw-alsa-lib

# Define LED configuration for WS2812
CFLAGS += -DCONFIG_LED_RGB_WS2812

include $(APPDIR)/Application.mk

 
