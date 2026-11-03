LVGL_PATH ?= ${shell pwd}/lvgl

ASRCS += $(shell find $(LVGL_PATH)/src -type f -name '*.S')
CSRCS += $(shell find $(LVGL_PATH)/src -type f -name '*.c')
CSRCS += $(shell find $(LVGL_PATH)/demos -type f -name '*.c')
CSRCS += $(shell find $(LVGL_PATH)/examples -type f -name '*.c')
CXXEXT := .cpp
CXXSRCS += $(shell find $(LVGL_PATH)/src -type f -name '*${CXXEXT}')

# ArtInChip GE2D/MPP backend; needs mpp_ge.h from vendor/artinchip BSP, absent
# on non-ArtInChip targets. Mirrors the gating in CMakeLists.txt.
ifneq ($(CONFIG_LV_USE_DRAW_GE2D),y)
ifneq ($(CONFIG_LV_USE_MPP_DEC),y)
CSRCS := $(filter-out $(LVGL_PATH)/src/draw/artinchip/%,$(CSRCS))
endif
endif

AFLAGS += "-I$(LVGL_PATH)"
CFLAGS += "-I$(LVGL_PATH)"
CXXFLAGS += "-I$(LVGL_PATH)" -Wno-shadow
