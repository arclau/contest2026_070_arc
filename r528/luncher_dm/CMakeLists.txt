# ##############################################################################
# apps/vendor/allwinnertech/apps/luncher_dm/CMakeLists.txt
#
# DesktopMate luncher_dm — CMake build entry (goldfish SIM / board).
# Source list mirrors Makefile CSRCS; sim/ stubs replace board HAL when
# CONFIG_LUNCHER_DM_SIM=y.
# ##############################################################################

if(CONFIG_LUNCHER_DM_APP)

  # 源清单唯一源：sources.txt（与 Makefile 共用，避免双轨漂移）。
  # sources.txt 为纯数据：一行一个 .c（相对本目录），勿加注释/空行。
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${CMAKE_CURRENT_SOURCE_DIR}/sources.txt")
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sources.txt" DM_CSRCS_COMMON)
  list(FILTER DM_CSRCS_COMMON EXCLUDE REGEX "^$")
  set(CSRCS ${DM_CSRCS_COMMON})

  if(CONFIG_LUNCHER_DM_SIM)
    list(APPEND CSRCS sim/dm_net_sim.c sim/dm_sim_stubs.c)
    # SIM 宏必须由构建系统显式传入：-isystem 顺序使 <nuttx/config.h> 命中
    # 源码树里的板端（R528）配置，C 代码读不到 Kconfig 的 CONFIG_LUNCHER_DM_SIM。
    # 用于 DM_RES_ROOT（SIM=/data/data，板端=/resource，见 dm_theme.h）。
    list(APPEND DM_SIM_DEFS CONFIG_LUNCHER_DM_SIM=1)
  else()
    list(APPEND CSRCS dm_net.c)
  endif()

  nuttx_add_application(
    NAME
    ${CONFIG_LUNCHER_DM_APP_PROGNAME}
    STACKSIZE
    ${CONFIG_LUNCHER_DM_APP_STACKSIZE}
    PRIORITY
    ${CONFIG_LUNCHER_DM_APP_PRIORITY}
    MODULE
    ${CONFIG_LUNCHER_DM_APP}
    SRCS
    luncher_dm.c
    ${CSRCS})

  target_include_directories(
    apps_luncher_dm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                            ${CMAKE_CURRENT_SOURCE_DIR}/sim/stubs
                            ${CMAKE_CURRENT_SOURCE_DIR}/common
                            # 截图 dm_screenshot.c：libpng/zlib 头（镜像 Makefile）
                            ${NUTTX_APPS_DIR}/external/libpng/libpng
                            ${NUTTX_APPS_DIR}/system/zlib/zlib)

  target_compile_definitions(apps_luncher_dm PRIVATE ${DM_SIM_DEFS})

  # SIM：NuttX math.h 在 LIBM_TOOLCHAIN 模式下不导出原型，统一补声明。
  if(CONFIG_LUNCHER_DM_SIM)
    target_compile_options(apps_luncher_dm PRIVATE
      -include ${CMAKE_CURRENT_SOURCE_DIR}/sim/dm_math_compat.h)
    # LVGL's blur code also uses toolchain libm; NuttX math.h omits expf here.
    target_compile_options(lvgl PRIVATE
      "$<$<COMPILE_LANGUAGE:C>:-include>"
      "$<$<COMPILE_LANGUAGE:C>:${CMAKE_CURRENT_SOURCE_DIR}/sim/dm_math_compat.h>")
  endif()

  # arm64 CMake 默认带 -Werror（nuttx/arch/arm64/src/cmake/gcc.cmake），板端 arm32 无。
  # 死代码已于 2026-10-15 清理（多日天气卡 / 首页音乐小组件 / UART 工具钮），
  # 不再需要 -Wno-error 降级；后续若再引入未调用 static 函数应直接删除，勿降级。

endif()
