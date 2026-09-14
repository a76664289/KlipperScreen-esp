/* 板型能力宏：板型知识只允许出现在 BSP 层。
 * 上层（ui / core / ports）需要按能力裁剪时，include 本头并判断能力宏，
 * 不得再直接判断 CONFIG_BOARD_*。新增板型时在下方登记其能力。 */
#pragma once

/* BSP_HAS_TOUCH_CAL：板型带电阻触摸层（XPT2046），支持两点校准，
 * 开放 CLI `caltouch` 强制重校；电容屏与无触摸板型为 0 */
#if defined(CONFIG_BOARD_CYD_2432S028R) || defined(CONFIG_BOARD_E32R35T) || \
    defined(CONFIG_BOARD_ESP32S3_ST7796_EC11)
#define BSP_HAS_TOUCH_CAL 1
#else
#define BSP_HAS_TOUCH_CAL 0
#endif

/* BSP_HAS_BUTTONS：板型注册了 GPIO 实体按钮导航后端（bsp_gpio_buttons，
   上/下/左/右/确定/返回喂 ui_buttons 语义层）；其余板型为 0，输入行为不变 */
#if defined(CONFIG_BOARD_ESP32S3_RETRO_GO)
#define BSP_HAS_BUTTONS 1
#else
#define BSP_HAS_BUTTONS 0
#endif
