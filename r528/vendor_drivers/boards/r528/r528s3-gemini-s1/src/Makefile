############################################################################
#
#   Copyright (C) 2013 Gregory Nutt. All rights reserved.
#   Author: Gregory Nutt <gnutt@nuttx.org>
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in
#    the documentation and/or other materials provided with the
#    distribution.
# 3. Neither the name NuttX nor the names of its contributors may be
#    used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
# FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
# COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
# OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
# AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
# ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#
############################################################################

include $(TOPDIR)/Make.defs
include $(TOPDIR)/../vendor/allwinnertech/Common.mk

ifneq ($(CONFIG_ALLWINNER_BINARY_ONLY),y)
CSRCS += r528_boot.c r528_leds.c
CSRCS += r528_appinit.c r528_bringup.c shim_binary_compat.c

# Use common HAL includes for board sources
$(eval $(AW_ADD_HAL_INCLUDES))
endif

# GT911 IIC Touchscreen Support
ifeq ($(CONFIG_GT911_IIC_TOUCH),y)
CSRCS += ../../drivers/gt911_iic_touch.c
# Note: HAL headers are centralized in chips/r528/Make.defs
CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)/arch/arm/include
endif

# GT9271 IIC Touchscreen Support
ifeq ($(CONFIG_GT9271_IIC_TOUCH),y)
CSRCS += ../../drivers/gt9271_iic_touch.c
# Note: HAL headers are centralized in chips/r528/Make.defs
CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)/arch/arm/include
endif

ifeq ($(CONFIG_ETC_ROMFS),y)
  RCSRCS += etc/init.d/rc.sysinit etc/init.d/rcS
 # RCRAWS += etc/key.avb
  RCRAWS += etc/build.prop
  RCRAWS += etc/factory.sh
 # RCRAWS += etc/rsa_public_key.pem
ifeq ($(CONFIG_X4B_AP),y)
  RCRAWS += etc/EQ_AD51652.conf
  RCRAWS += etc/EQ_ACM.conf
  RCRAWS += etc/EQ_AD52058.conf
  RCRAWS += etc/usr/share/mico
  ifeq ($(CONFIG_MICO_DEFAULT_TTS_PACK_INTO_FIRMWARE),y)
    RCRAWS += etc/usr/share/sound/common
    RCRAWS += etc/usr/share/sound/vendor
  else
    TTS_BASE_DIR = etc/usr/share/sound
    TTS_COPY_DIR = ${TOPDIR}/../vendor/allwinnertech/lichee/board/common/data/res
    ${shell rm -rf ${TTS_COPY_DIR}/${TTS_BASE_DIR}}
    ${shell mkdir -p ${TTS_COPY_DIR}/${TTS_BASE_DIR}}
    ${shell cp -r ${TTS_BASE_DIR}/* ${TTS_COPY_DIR}/${TTS_BASE_DIR}}
  endif
endif
ifeq ($(CONFIG_X4B_FACTEST),y)
  RCRAWS += etc/EQ_AD51652.conf
  RCRAWS += etc/EQ_ACM.conf
  RCRAWS += etc/EQ_AD52058.conf
  RCRAWS += etc/wifi_factest/rtl8733bs_wifi.map
  RCRAWS += etc/wifi_factest/rtl8733bs_wifi.mask
endif
endif

ifeq ($(CONFIG_LIBC_LOCALTIME),y)
RCRAWS += etc/zoneinfo
endif

ifeq ($(CONFIG_SERVICES_MICO_ALARM),y)
RCRAWS += etc/workday
endif

ifeq ($(CONFIG_MIDESS),y)
RCRAWS += etc/mi/ui_support.json
endif

ifeq ($(CONFIG_MEDIA_SERVER),y)
RCRAWS += etc/media
endif

ifeq ($(CONFIG_COMPONENTS_BLUETOOTH),y)
ifeq ($(CONFIG_X4B_FACTEST),y)
  RCRAWS += etc/bt_factest
else
  RCRAWS += etc/bt
endif
endif

ifeq ($(CONFIG_SERVICES_PNS),y)
  PNS_BASE_DIR = ${TOPDIR}/../vendor/xiaomi/miai/pns/thirdparty/$(CONFIG_PNS_PRODUCT_SERIES)
  PNS_DST_DIR = etc/pns/configs/vpm/xiaomi
  ifneq ($(CONFIG_PNS_LIBVPM_ENABLE),)
    ${shell rm -rf etc/pns}
    ${shell mkdir -p ${PNS_DST_DIR}}
    ${shell cp -r ${PNS_BASE_DIR}/vpm/configs/xiaomi/* ${PNS_DST_DIR}}
    ifneq ($(CONFIG_PNS_CFG_PACK_INTO_FIRMWARE),)
      RCRAWS += ${PNS_DST_DIR}
    else
      PNS_COPY_DIR = ${TOPDIR}/../vendor/allwinnertech/lichee/board/common/data/res
      ${shell rm -rf ${PNS_COPY_DIR}/etc/pns}
      ${shell mkdir -p ${PNS_COPY_DIR}/${PNS_DST_DIR}}
      ifeq ($(wildcard etc/pns),)
        ${shell cp -r ${PNS_BASE_DIR}/vpm/configs/xiaomi/* ${PNS_COPY_DIR}/${PNS_DST_DIR}}
      else
        ${shell cp -r ${PNS_DST_DIR}/* ${PNS_COPY_DIR}/${PNS_DST_DIR}}
      endif
    endif
  endif
endif

ifeq ($(CONFIG_SYSTEM_PACKAGE_SERVICE), y)
RCRAWS += etc/package.cfg
endif

ifeq ($(CONFIG_SYSTEM_ACTIVITY_SERVICE), y)
RCRAWS += etc/lmk.cfg
endif

.PHONY: FORCE

FORCE:

ARCH_BOARD_CUSTOM_DIR := $(subst ",,${CONFIG_ARCH_BOARD_CUSTOM_DIR})
ARCH_BOARD_CUSTOM_NAME := $(subst ",,${CONFIG_ARCH_BOARD_CUSTOM_NAME})

ifeq ($(CONFIG_TA_ALIPAY),y)
ifneq ($(CONFIG_INTERPRETERS_WAMR),)
ifneq ($(wildcard $(APPDIR)/bin/wasm/ta_alipay.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/F1490647578C565BA2C321FD51DC6791: FORCE
	cp $(APPDIR)/bin/wasm/ta_alipay.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/F1490647578C565BA2C321FD51DC6791
	cp $(APPDIR)/bin/wasm/ta_alipay.xip $(APPDIR)/staging/ta_alipay.xip
else
ifneq ($(wildcard $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_alipay.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/F1490647578C565BA2C321FD51DC6791: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	cp $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_alipay.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/F1490647578C565BA2C321FD51DC6791
else
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/F1490647578C565BA2C321FD51DC6791: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	touch $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/F1490647578C565BA2C321FD51DC6791
endif
endif
  RCRAWS += etc/ta/F1490647578C565BA2C321FD51DC6791
endif
endif

ifeq ($(CONFIG_TA_COMSST),y)
ifneq ($(CONFIG_INTERPRETERS_WAMR),)
ifneq ($(wildcard $(APPDIR)/bin/wasm/ta_comsst.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/F617ACC99AE5405985D05056DF6BF28B: FORCE
	cp $(APPDIR)/bin/wasm/ta_comsst.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/F617ACC99AE5405985D05056DF6BF28B
	cp $(APPDIR)/bin/wasm/ta_comsst.xip $(APPDIR)/staging/ta_comsst.xip
else
ifneq ($(wildcard $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_comsst.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/F617ACC99AE5405985D05056DF6BF28B: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	cp $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_comsst.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/F617ACC99AE5405985D05056DF6BF28B
else
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/F617ACC99AE5405985D05056DF6BF28B: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	touch $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/F617ACC99AE5405985D05056DF6BF28B
endif
endif
  RCRAWS += etc/ta/F617ACC99AE5405985D05056DF6BF28B
endif
endif

ifeq ($(CONFIG_TA_HELLO_WORLD),y)
ifneq ($(CONFIG_INTERPRETERS_WAMR),)
ifneq ($(wildcard $(APPDIR)/bin/wasm/ta_hello_world.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/00F2AA8A5024E411ABE20002A5D5C51B: FORCE
	cp $(APPDIR)/bin/wasm/ta_hello_world.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/00F2AA8A5024E411ABE20002A5D5C51B
	cp $(APPDIR)/bin/wasm/ta_hello_world.xip $(APPDIR)/staging/ta_hello_world.xip
else
ifneq ($(wildcard $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_hello_world.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/00F2AA8A5024E411ABE20002A5D5C51B: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	cp $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_hello_world.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/00F2AA8A5024E411ABE20002A5D5C51B
else
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/00F2AA8A5024E411ABE20002A5D5C51B: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	touch $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/00F2AA8A5024E411ABE20002A5D5C51B
endif
endif
  RCRAWS += etc/ta/00F2AA8A5024E411ABE20002A5D5C51B
endif
endif

ifeq ($(CONFIG_TA_TRIAD),y)
ifneq ($(CONFIG_INTERPRETERS_WAMR),)
ifneq ($(wildcard $(APPDIR)/bin/wasm/ta_triad.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/1C6455C967DD255194BE7F9DCD557595: FORCE
	cp $(APPDIR)/bin/wasm/ta_triad.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/1C6455C967DD255194BE7F9DCD557595
	cp $(APPDIR)/bin/wasm/ta_triad.xip $(APPDIR)/staging/ta_triad.xip
else
ifneq ($(wildcard $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_triad.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/1C6455C967DD255194BE7F9DCD557595: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	cp $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_triad.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/1C6455C967DD255194BE7F9DCD557595
else
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/1C6455C967DD255194BE7F9DCD557595: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	touch $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/1C6455C967DD255194BE7F9DCD557595
endif
endif
  RCRAWS += etc/ta/1C6455C967DD255194BE7F9DCD557595
endif
endif

ifeq ($(CONFIG_TA_WXCODEPAY),y)
ifneq ($(CONFIG_INTERPRETERS_WAMR),)
ifneq ($(wildcard $(APPDIR)/bin/wasm/ta_wxcodepay.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/4EBA9EA8ACF42751AD93A5A4F03E1A67: FORCE
	cp $(APPDIR)/bin/wasm/ta_wxcodepay.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/4EBA9EA8ACF42751AD93A5A4F03E1A67
	cp $(APPDIR)/bin/wasm/ta_wxcodepay.xip $(APPDIR)/staging/ta_wxcodepay.xip
else
ifneq ($(wildcard $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_wxcodepay.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/4EBA9EA8ACF42751AD93A5A4F03E1A67: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	cp $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_wxcodepay.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/4EBA9EA8ACF42751AD93A5A4F03E1A67
else
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/4EBA9EA8ACF42751AD93A5A4F03E1A67: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	touch $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/4EBA9EA8ACF42751AD93A5A4F03E1A67
endif
endif
  RCRAWS += etc/ta/4EBA9EA8ACF42751AD93A5A4F03E1A67
endif
endif

ifeq ($(CONFIG_TA_PIN),y)
ifneq ($(CONFIG_INTERPRETERS_WAMR),)
ifneq ($(wildcard $(APPDIR)/bin/wasm/ta_pin.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/EE571882BE621355ADAB1B7F23DEFAA6: FORCE
	cp $(APPDIR)/bin/wasm/ta_pin.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/EE571882BE621355ADAB1B7F23DEFAA6
	cp $(APPDIR)/bin/wasm/ta_pin.xip $(APPDIR)/staging/ta_pin.xip
else
ifneq ($(wildcard $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_pin.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/EE571882BE621355ADAB1B7F23DEFAA6: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	cp $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_pin.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/EE571882BE621355ADAB1B7F23DEFAA6
else
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/EE571882BE621355ADAB1B7F23DEFAA6: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	touch $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/EE571882BE621355ADAB1B7F23DEFAA6
endif
endif
  RCRAWS += etc/ta/EE571882BE621355ADAB1B7F23DEFAA6
endif
endif

ifeq ($(CONFIG_TA_KEYSTORE),y)
ifneq ($(CONFIG_INTERPRETERS_WAMR),)
ifneq ($(wildcard $(APPDIR)/bin/wasm/ta_keystore.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/88A89F14B906EF4EBD96940EFFDABEF4: FORCE
	cp $(APPDIR)/bin/wasm/ta_keystore.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/88A89F14B906EF4EBD96940EFFDABEF4
	cp $(APPDIR)/bin/wasm/ta_keystore.xip $(APPDIR)/staging/ta_keystore.xip
else
ifneq ($(wildcard $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_keystore.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/88A89F14B906EF4EBD96940EFFDABEF4: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	cp $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_keystore.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/88A89F14B906EF4EBD96940EFFDABEF4
else
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/88A89F14B906EF4EBD96940EFFDABEF4: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	touch $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/88A89F14B906EF4EBD96940EFFDABEF4
endif
endif
  RCRAWS += etc/ta/88A89F14B906EF4EBD96940EFFDABEF4
endif
endif

ifeq ($(CONFIG_OPTEE_TEST_TA),y)
ifneq ($(CONFIG_INTERPRETERS_WAMR),)
ifneq ($(wildcard $(APPDIR)/bin/wasm/ta_crypt.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/A05B3ECBF1ADE011998B0002A5D5C51B: FORCE
	cp $(APPDIR)/bin/wasm/ta_crypt.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/A05B3ECBF1ADE011998B0002A5D5C51B
	cp $(APPDIR)/bin/wasm/ta_crypt.xip $(APPDIR)/staging/ta_crypt.xip
else
ifneq ($(wildcard $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_crypt.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/A05B3ECBF1ADE011998B0002A5D5C51B: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	cp $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_crypt.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/A05B3ECBF1ADE011998B0002A5D5C51B
else
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/A05B3ECBF1ADE011998B0002A5D5C51B: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	touch $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/A05B3ECBF1ADE011998B0002A5D5C51B
endif
endif
  RCRAWS += etc/ta/A05B3ECBF1ADE011998B0002A5D5C51B

ifneq ($(wildcard $(APPDIR)/bin/wasm/ta_storage.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/A7F289B6DF8A7A479F9932E90C0AD0A2: FORCE
	cp $(APPDIR)/bin/wasm/ta_storage.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/A7F289B6DF8A7A479F9932E90C0AD0A2
	cp $(APPDIR)/bin/wasm/ta_storage.xip $(APPDIR)/staging/ta_storage.xip
else
ifneq ($(wildcard $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_storage.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/A7F289B6DF8A7A479F9932E90C0AD0A2: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	cp $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_storage.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/A7F289B6DF8A7A479F9932E90C0AD0A2
else
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/A7F289B6DF8A7A479F9932E90C0AD0A2: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	touch $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/A7F289B6DF8A7A479F9932E90C0AD0A2
endif
endif
  RCRAWS += etc/ta/A7F289B6DF8A7A479F9932E90C0AD0A2

ifneq ($(wildcard $(APPDIR)/bin/wasm/ta_storage2.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/9E271E73FBAA7545A77138CAA6F0CCA6: FORCE
	cp $(APPDIR)/bin/wasm/ta_storage2.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/9E271E73FBAA7545A77138CAA6F0CCA6
	cp $(APPDIR)/bin/wasm/ta_storage2.xip $(APPDIR)/staging/ta_storage2.xip
else
ifneq ($(wildcard $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_storage2.xip),)
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/9E271E73FBAA7545A77138CAA6F0CCA6: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	cp $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/libs/$(ARCH_BOARD_CUSTOM_NAME)/ta_storage2.xip $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/9E271E73FBAA7545A77138CAA6F0CCA6
else
$(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/9E271E73FBAA7545A77138CAA6F0CCA6: FORCE
	mkdir -p $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta
	touch $(TOPDIR)/$(ARCH_BOARD_CUSTOM_DIR)/src/etc/ta/9E271E73FBAA7545A77138CAA6F0CCA6
endif
endif
  RCRAWS += etc/ta/9E271E73FBAA7545A77138CAA6F0CCA6

endif
endif


ifeq ($(CONFIG_ALLWINNER_BINARY_ONLY),y)
all: libboard$(LIBEXT)
context:
depend:
clean:
	$(call DELFILE, libboard$(LIBEXT))
distclean: clean
	$(call DELFILE, Make.dep)
	$(call DELFILE, .depend)

libboard$(LIBEXT):
	$(Q) cp $(TOPDIR)/../vendor/allwinnertech/libs/board/r528s3-gemini-s1/libboard_allwinner$(LIBEXT) $@
else
include $(TOPDIR)/boards/Board.mk
endif

ifeq ($(CONFIG_ALLWINNER_BINARY_ONLY),y)
EXTRA_LIBPATHS += -L$(TOPDIR)/../vendor/allwinnertech/libs/drivers/r528
EXTRA_LIBS     += -ldrivers_r528
endif

# Board-specific include paths (AW_HAL_DIR is defined in chips/r528/Make.defs)
AW_SRCDIR = $(TOPDIR)/arch/arm/src/chip
CFLAGS += ${INCDIR_PREFIX}$(AW_SRCDIR)
CFLAGS += ${INCDIR_PREFIX}$(AW_SRCDIR)/include/drivers


RCRAWS += etc/lvx_video_config.json
ifeq ($(CONFIG_BLUETOOTH),y)
RCRAWS += etc/bt/8723fs_btaddr.txt
RCRAWS += etc/bt/8723fs_config_C-cut.bin
RCRAWS += etc/bt/8723fs_fw_C-cut.bin
endif

ifeq ($(CONFIG_IEEE80211_REALTEK_WIFI),y)
RCRAWS += etc/wifi/start_wifi.sh
endif

ifeq ($(CONFIG_UTILS_SSH),y)
RCRAWS += etc/ssh/ssh_config
RCRAWS += etc/ssh/ssh_known_hosts
endif
