#!/usr/bin/env bash
set -euo pipefail
cd /data/dm
cc -std=c11 -D_POSIX_C_SOURCE=200809L -g -O1 -Wall -Wextra -Werror -Wno-sign-compare -Wno-misleading-indentation -Wno-unused-parameter -fsanitize=address,undefined -fno-omit-frame-pointer -DLV_CONF_PATH=/data/dm/output/p304-pet-rework/host/lv_conf.h '-DPET_ASSET_ROOT="/data/dm/vendor/allwinnertech/apps/luncher_dm/assets/pet"' -Ioutput/p304-pet-rework/host -Iapps/graphics/lvgl/lvgl -Ivendor/allwinnertech/apps/luncher_dm -Iapps/include -Iapps/netutils/cjson/cJSON -I/usr/include/freetype2 contest2026_070_arc/tools/tests/pet_time/test_time.c vendor/allwinnertech/apps/luncher_dm/pet_actions.c apps/netutils/cjson/cJSON/cJSON.c output/p304-pet-rework/host/native-build/liblvgl.a -lfreetype -lpng -lz -lm -lpthread -o output/p334-voice-guard/test_time
ASAN_OPTIONS=detect_leaks=0 output/p334-voice-guard/test_time
