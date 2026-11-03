if(CONFIG_DISP2_SUNXI)
    if(CONFIG_ARCH_BOARD_R528S3_EVB2)
        list(APPEND SRCS soc/gc9503cv_mipi_config_evb2.c)
    endif()
    if(CONFIG_ARCH_BOARD_R528S3_EVB4)
        list(APPEND SRCS soc/gc9503cv_mipi_config_evb4.c)
    endif()
    if(CONFIG_ARCH_BOARD_R528S3_X4B)
        list(APPEND SRCS soc/gc9503cv_mipi_config_x4b.c)
    endif()
    if(CONFIG_DISP2_SUNXI_SUPPORT_ENAHNCE)
        list(APPEND SRCS disp/de/disp_enhance.c)
    endif()
    if(CONFIG_DISP2_SUNXI_SUPPORT_SMBL)
        list(APPEND SRCS disp/de/disp_smart_backlight.c)
    endif()
    if(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
        list(APPEND SRCS disp/fb_g2d_rot.c)
    endif()
    if(CONFIG_LCD_SUPPORT_GC9503CV)
        list(APPEND SRCS disp/lcd/gc9503cv.c)
    endif()
    if(CONFIG_LCD_SUPPORT_ST7701S)
        list(APPEND SRCS disp/lcd/st7701s.c)
    endif()
	if(CONFIG_LCD_SUPPORT_TFT08006)
		list(APPEND SRCS disp/lcd/tft08006.c)
	endif()

	if(CONFIG_VIDEO_FB)
        list(APPEND SRCS disp/dev_fb.c)
    endif()
	
	list(APPEND SRCS 
		soc/sun8iw20.c 
		soc/disp_board_config.c 
		soc/platform_resource.c 
		disp/dev_disp.c 
		disp/disp_sys_intf.c 
		disp/de/disp_display.c 
		disp/de/disp_features.c 
		disp/de/disp_device.c 
		disp/de/disp_lcd.c 
		disp/de/disp_private.c 
		disp/de/disp_capture.c 
		disp/de/disp_tv.c 
		disp/de/lowlevel_v2x/de_dsi.c 
		disp/de/lowlevel_v2x/de_wb.c 
		disp/de/lowlevel_v2x/de_lcd_sun50iw10.c 
		disp/de/lowlevel_v2x/de_fcc.c 
		disp/de/lowlevel_v2x/de_gsu.c 
		disp/de/lowlevel_v2x/de_peak.c 
		disp/de/lowlevel_v2x/de_rtmx.c 
		disp/de/lowlevel_v2x/de_scaler_table.c 
		disp/de/lowlevel_v2x/de_vep_table.c 
		disp/de/lowlevel_v2x/de_vsu.c 
		disp/de/lowlevel_v2x/de_feat.c 
		disp/de/lowlevel_v2x/de_lti.c 
		disp/de/lowlevel_v2x/de_fce.c 
		disp/de/lowlevel_v2x/de_ase.c 
		disp/de/lowlevel_v2x/de_bws.c 
		disp/de/lowlevel_v2x/de_smbl.c 
		disp/de/lowlevel_v2x/de_ccsc.c 
		disp/de/lowlevel_v2x/de_dcsc.c 
		disp/de/lowlevel_v2x/de_clock.c 
		disp/de/lowlevel_v2x/de_enhance.c
		disp/de/lowlevel_v2x/de_hal.c
		disp/de/lowlevel_v2x/disp_al.c 
		disp/de/disp_manager.c 	
		disp/lcd/panels.c 
		disp/lcd/lcd_source.c 
		disp/lcd/default_panel.c
	)
	
endif()

#nuttx_add_library(disp2 ${SRCS})
target_include_directories(
    arch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/soc
	${CMAKE_CURRENT_SOURCE_DIR}/../../../include/hal/
	${CMAKE_CURRENT_SOURCE_DIR}/../../../include/osal/
	${CMAKE_CURRENT_SOURCE_DIR}/../
	${CMAKE_CURRENT_SOURCE_DIR}/../g2d_rcq
)

target_sources(arch PRIVATE ${SRCS})
