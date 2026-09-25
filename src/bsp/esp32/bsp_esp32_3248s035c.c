/*
 * BSP: ESP32-3248S035C（ESP32，3.5" 320x480 ST7796 SPI + GT911 电容触摸）
 * 横屏使用：LVGL 分辨率 480x320，面板 swap_xy
 *
 * 引脚：
 *   TFT: MISO=12, MOSI=13, SCLK=14, CS=15, DC=2, RST=-1(不用), BL=27
 *   TP:  SDA=33, SCL=32, INT=-1(不用), RST=25
 *   BOOT 键: GPIO0
 *
 * 无 PSRAM → LVGL 用 partial buffer（DMA 内存）
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_ESP32_3248S035C

#include "bsp.h"
#include "bsp_screen_power.h"
#include "bsp_sleep_button.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_lcd_st7796.h"

/* ---------- 引脚 ---------- */
#define PIN_LCD_MISO   12
#define PIN_LCD_MOSI   13
#define PIN_LCD_SCLK   14
#define PIN_LCD_CS     15
#define PIN_LCD_DC     2
#define PIN_LCD_RST    (-1)
#define PIN_LCD_BL     27

#define PIN_TP_SDA     33
#define PIN_TP_SCL     32
#define PIN_TP_RST     25
#define PIN_TP_INT     (-1)

#define PIN_BTN_BOOT   0

#define LCD_H_RES      480
#define LCD_V_RES      320
#define LCD_PCLK_HZ    (40 * 1000 * 1000)

static const char *TAG = "bsp";

static SemaphoreHandle_t lvgl_mux;
static esp_lcd_panel_io_handle_t io_handle;
static esp_lcd_panel_handle_t panel_handle;
static esp_lcd_touch_handle_t touch_handle;
static lv_color_t *disp_draw_buf;
static uint16_t *lcd_push_buf;
static SemaphoreHandle_t lcd_trans_done;
static bool disp_rot180;
static bool disp_mirrorx;

/* ---------- 背光 ---------- */
static uint8_t bl_duty = 255;

static void backlight_apply(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    bl_duty = (uint8_t)(pct * 255 / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, bl_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static uint64_t screen_now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

void bsp_lvgl_lock(void)   { xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY); }
void bsp_lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }
void bsp_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void bsp_fade_out(uint32_t ms)
{
    static bool inst;
    if (!inst) { ledc_fade_func_install(0); inst = true; }
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0, ms);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
    bl_duty = 0;
}

bool bsp_disp_can_invert(void)    { return true; }
bool bsp_disp_can_rotate180(void) { return true; }
bool bsp_disp_can_mirror_x(void)  { return true; }

void bsp_disp_set_invert(bool en)
{
    if (panel_handle) esp_lcd_panel_invert_color(panel_handle, en);
}

void bsp_disp_set_rotate180(bool en)
{
    disp_rot180 = en;
    if (panel_handle)
        esp_lcd_panel_mirror(panel_handle,
                             disp_rot180 != disp_mirrorx,
                             disp_rot180);
}

void bsp_disp_set_mirror_x(bool en)
{
    disp_mirrorx = en;
    if (panel_handle)
        esp_lcd_panel_mirror(panel_handle,
                             disp_rot180 != disp_mirrorx,
                             disp_rot180);
}

/* ---------- LVGL ---------- */
/* SPI 传输完成回调：此时才告诉 LVGL buffer 可以复用 */
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                 esp_lcd_panel_io_event_data_t *edata,
                                 void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(lcd_trans_done, &high_task_woken);
    lv_display_flush_ready(disp);
    return high_task_woken == pdTRUE;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* ST7796 走 SPI 要求先发像素高字节，LVGL 内存是小端 RGB565 → 就地交换字节 */
    uint16_t *p = (uint16_t *)px_map;
    int32_t n = (int32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    for (int32_t i = 0; i < n; i++)
        p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));

    esp_lcd_panel_draw_bitmap(panel_handle,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              px_map);
    /* 不在这里调 lv_display_flush_ready —— 由 on_color_trans_done 回调调 */
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool wake_swallow;
    esp_lcd_touch_point_data_t pt[1] = {0};
    uint8_t count = 0;
    esp_lcd_touch_read_data(touch_handle);
    if (esp_lcd_touch_get_data(touch_handle, pt, &count, 1) == ESP_OK && count > 0) {
        if (bsp_screen_activity()) wake_swallow = true;
        if (wake_swallow) { data->state = LV_INDEV_STATE_RELEASED; return; }
        data->state = LV_INDEV_STATE_PRESSED;
        int x = LV_CLAMP(0, pt[0].x, LCD_H_RES - 1);
        int y = LV_CLAMP(0, pt[0].y, LCD_V_RES - 1);
        if (disp_rot180 || disp_mirrorx) x = LCD_H_RES - 1 - x;
        if (disp_rot180) y = LCD_V_RES - 1 - y;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        wake_swallow = false;
    }
}

static void lvgl_task(void *arg)
{
    for (;;) {
        bsp_lvgl_lock();
        lv_timer_handler();
        bsp_screen_power_poll();
        bsp_sleep_button_poll();
        bsp_lvgl_unlock();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

lv_display_t *bsp_get_display(void) { return lv_display_get_default(); }

/* ---------- 初始化 ---------- */
void bsp_init(void)
{
    lvgl_mux = xSemaphoreCreateRecursiveMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_vfs_littlefs_conf_t fs_conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs_conf));

    /* 背光 */
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ledc_channel_config_t bl_ch = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 255,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));
    bsp_screen_power_init(backlight_apply, screen_now_ms);

    /* SPI 总线 */
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_LCD_MISO,
        .mosi_io_num = PIN_LCD_MOSI,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* LCD IO */
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                             &io_config, &io_handle));

    /* ST7796 面板：BGR（对齐三块参考板） */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    /* 保持与运行时默认设置一致，避免开机 logo 与 UI 方向不同。 */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    /* 触摸 GT911 */
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 320,
        .y_max = 480,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = true, .mirror_x = true, .mirror_y = false },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &touch_handle));

    /* LVGL */
    lv_init();
    lv_tick_set_cb(tick_cb);

    size_t buf_px = LCD_H_RES * 40;
    disp_draw_buf = heap_caps_malloc(buf_px * sizeof(lv_color_t),
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!disp_draw_buf) {
        ESP_LOGE(TAG, "LVGL buffer alloc failed");
        return;
    }

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_buffers(disp, disp_draw_buf, NULL,
                           buf_px * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    /* 注册 SPI 完成回调（disp 作为 user_ctx） */
    lcd_trans_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(lcd_trans_done ? ESP_OK : ESP_ERR_NO_MEM);
    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, disp));

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    const bsp_sleep_button_cfg_t sleep_btns[] = {{ PIN_BTN_BOOT, true }};
    ESP_ERROR_CHECK(bsp_sleep_button_init(sleep_btns, 1));

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "BSP ready (ESP32-3248S035C, %dx%d landscape, ST7796 SPI + GT911)",
             LCD_H_RES, LCD_V_RES);
}

void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *px)
{
    if (!panel_handle) return;
    size_t count = (size_t)w * h;
    if (count > (size_t)LCD_H_RES * 40) return;
    if (!lcd_push_buf) {
        lcd_push_buf = heap_caps_malloc((size_t)LCD_H_RES * 40 * sizeof(uint16_t),
                                        MALLOC_CAP_DMA);
        if (!lcd_push_buf) return;
    }
    xSemaphoreTake(lcd_trans_done, 0);
    for (size_t i = 0; i < count; i++)
        lcd_push_buf[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, lcd_push_buf);
    xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(100));
}

void bsp_lcd_stats_print(void) { }

void bsp_restart(void)
{
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

#endif /* CONFIG_BOARD_ESP32_3248S035C */