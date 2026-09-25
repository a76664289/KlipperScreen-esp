/*
 * BSP: JC4827W543C（ESP32-S3-WROOM-1，4.3" 480x272 QSPI ST3401A/NV3041A + GT911）
 * 引脚与 QSPI 时序取自厂商 Arduino 例程（ESP32_4827A043_QSPI）。
 * 
 * QSPI 时序严格照 Arduino_ESP32QSPI.cpp：
 *   - 命令帧：cmd=0x02, addr=(面板命令<<8)，参数用 tx_data（≤4B）或 tx_buffer
 *   - 数据帧：第一帧 cmd=0x32, addr=0x003C00；后续帧带 VARIABLE_CMD/ADDR 不带 cmd/addr
 *   - 像素数据必须先 memcpy 到 16 字节对齐的 DMA 缓冲，再交给 tx_buffer
 *   - 全程一个 CS 周期，用 spi_device_polling_start/end（transmit 会忽略 ext 字段）
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_JC4827W543C

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
#include "esp_lcd_touch_gt911.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/* ---------- 引脚定义 ---------- */
#define PIN_LCD_CS     45
#define PIN_LCD_SCK    47
#define PIN_LCD_D0     21
#define PIN_LCD_D1     48
#define PIN_LCD_D2     40
#define PIN_LCD_D3     39
#define PIN_LCD_BL     1

#define PIN_TP_SDA     8
#define PIN_TP_SCL     4
#define PIN_TP_RST     38
#define PIN_TP_INT     3
#define PIN_BTN_BOOT   0

#define LCD_H_RES      480
#define LCD_V_RES      272
#define LCD_PCLK_HZ    (32 * 1000 * 1000)

/* QSPI 命令码 */
#define QSPI_CMD_WRITE  0x02
#define QSPI_CMD_DATA   0x32
#define QSPI_DATA_ADDR  0x003C00

#define QSPI_MAX_PIXELS 1024

/* 面板命令 */
#define NV3041A_CASET  0x2A
#define NV3041A_RASET  0x2B
#define NV3041A_RAMWR  0x2C
#define NV3041A_MADCTL 0x36

static const char *TAG = "bsp";

static SemaphoreHandle_t lvgl_mux;
static spi_device_handle_t qspi_dev;
static esp_lcd_touch_handle_t touch_handle;
static lv_color_t *disp_draw_buf;

/* QSPI 像素数据 DMA 对齐缓冲 */
static uint16_t *qspi_tx_buf = NULL;

/* ---------- CS 手动控制 ---------- */
static inline void cs_low(void)  { gpio_set_level(PIN_LCD_CS, 0); }
static inline void cs_high(void) { gpio_set_level(PIN_LCD_CS, 1); }

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

/* ---------- QSPI 底层：发命令 / 发像素 ---------- */

/* 发一条面板命令 + 0..N 字节参数。
 * 参数 ≤ 4 字节：用 SPI_TRANS_USE_TXDATA（不经 DMA，避免对齐问题）
 * 参数 > 4 字节：用 tx_buffer（本 BSP 命令都 ≤ 4 字节，用不到） */
static void nv3041a_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    spi_transaction_ext_t t;

    memset(&t, 0, sizeof(t));
    t.base.cmd    = QSPI_CMD_WRITE;
    t.base.addr   = ((uint32_t)cmd) << 8;
    t.base.length = len * 8;
    t.base.flags  = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;

    if (len == 0) {
        t.base.tx_buffer = NULL;
    } else if (len <= 4) {
        t.base.flags |= SPI_TRANS_USE_TXDATA;
        memcpy(t.base.tx_data, data, len);
    } else {
        t.base.tx_buffer = data;
    }

    cs_low();
    ESP_ERROR_CHECK(spi_device_polling_start(qspi_dev,
                                              (spi_transaction_t *)&t,
                                              portMAX_DELAY));
    ESP_ERROR_CHECK(spi_device_polling_end(qspi_dev, portMAX_DELAY));
    cs_high();
}

/* 发像素数据：第一帧带 cmd/addr，后续帧带 VARIABLE_CMD/ADDR 不带 cmd/addr，
 * 整段像素一个 CS 周期。数据先拷到 16 字节对齐的 qspi_tx_buf。 */
static void nv3041a_push_pixels(const uint16_t *px, size_t n)
{
    if (n == 0 || qspi_tx_buf == NULL) return;

    spi_transaction_ext_t t;
    size_t remain = n;
    const uint16_t *p = px;
    bool first = true;

    cs_low();

    while (remain > 0) {
        size_t chunk = (remain > QSPI_MAX_PIXELS) ? QSPI_MAX_PIXELS : remain;

        /* 拷到对齐缓冲（Arduino 做法，ESP32-S3 SPI DMA 要求 4B 对齐） */
        memcpy(qspi_tx_buf, p, chunk * 2);

        memset(&t, 0, sizeof(t));
        t.base.length    = chunk * 16;
        t.base.tx_buffer = qspi_tx_buf;

        if (first) {
            t.base.flags = SPI_TRANS_MODE_QIO;
            t.base.cmd   = QSPI_CMD_DATA;
            t.base.addr  = QSPI_DATA_ADDR;
            first = false;
        } else {
            t.base.flags = SPI_TRANS_MODE_QIO
                         | SPI_TRANS_VARIABLE_CMD
                         | SPI_TRANS_VARIABLE_ADDR
                         | SPI_TRANS_VARIABLE_DUMMY;
        }

        ESP_ERROR_CHECK(spi_device_polling_start(qspi_dev,
                                                  (spi_transaction_t *)&t,
                                                  portMAX_DELAY));
        ESP_ERROR_CHECK(spi_device_polling_end(qspi_dev, portMAX_DELAY));

        p      += chunk;
        remain -= chunk;
    }

    cs_high();
}

/* 设置区域窗口 */
static void nv3041a_set_window(int x1, int y1, int x2, int y2)
{
    uint8_t caset[4] = {
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
        (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF),
    };
    uint8_t raset[4] = {
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
        (uint8_t)(y2 >> 8), (uint8_t)(y2 & 0xFF),
    };
    nv3041a_cmd(NV3041A_CASET, caset, 4);
    nv3041a_cmd(NV3041A_RASET, raset, 4);
}

/* ---------- NV3041A 完整初始化 ---------- */
struct nv_cmd {
    uint8_t  cmd;
    uint8_t  data[4];
    uint8_t  len;
    uint16_t delay_ms;
};

static const struct nv_cmd nv3041a_init_cmds[] = {
    {0x01, {0}, 0, 120},
    {0xFF, {0xA5}, 1, 0},
    {0x36, {0xC0}, 1, 0},
    {0x3A, {0x01}, 1, 0},
    {0x41, {0x03}, 1, 0},
    {0x44, {0x15}, 1, 0},
    {0x45, {0x15}, 1, 0},
    {0x7D, {0x03}, 1, 0},
    {0xC1, {0xAB}, 1, 0},
    {0xC2, {0x17}, 1, 0},
    {0xC3, {0x10}, 1, 0},
    {0xC6, {0x3A}, 1, 0},
    {0xC7, {0x25}, 1, 0},
    {0xC8, {0x11}, 1, 0},
    {0x7A, {0x49}, 1, 0},
    {0x6F, {0x2F}, 1, 0},
    {0x78, {0x4B}, 1, 0},
    {0xC9, {0x00}, 1, 0},
    {0x67, {0x33}, 1, 0},
    {0x51, {0x4B}, 1, 0},
    {0x52, {0x7C}, 1, 0},
    {0x53, {0x1C}, 1, 0},
    {0x54, {0x77}, 1, 0},
    {0x46, {0x0A}, 1, 0},
    {0x47, {0x2A}, 1, 0},
    {0x48, {0x0A}, 1, 0},
    {0x49, {0x1A}, 1, 0},
    {0x56, {0x43}, 1, 0},
    {0x57, {0x42}, 1, 0},
    {0x58, {0x3C}, 1, 0},
    {0x59, {0x64}, 1, 0},
    {0x5A, {0x41}, 1, 0},
    {0x5B, {0x3C}, 1, 0},
    {0x5C, {0x02}, 1, 0},
    {0x5D, {0x3C}, 1, 0},
    {0x5E, {0x1F}, 1, 0},
    {0x60, {0x80}, 1, 0},
    {0x61, {0x3F}, 1, 0},
    {0x62, {0x21}, 1, 0},
    {0x63, {0x07}, 1, 0},
    {0x64, {0xE0}, 1, 0},
    {0x65, {0x01}, 1, 0},
    {0xCA, {0x20}, 1, 0},
    {0xCB, {0x52}, 1, 0},
    {0xCC, {0x10}, 1, 0},
    {0xCD, {0x42}, 1, 0},
    {0xD0, {0x20}, 1, 0},
    {0xD1, {0x52}, 1, 0},
    {0xD2, {0x10}, 1, 0},
    {0xD3, {0x42}, 1, 0},
    {0xD4, {0x0A}, 1, 0},
    {0xD5, {0x32}, 1, 0},
    {0x80, {0x04}, 1, 0}, {0xA0, {0x00}, 1, 0},
    {0x81, {0x07}, 1, 0}, {0xA1, {0x05}, 1, 0},
    {0x82, {0x06}, 1, 0}, {0xA2, {0x04}, 1, 0},
    {0x86, {0x2C}, 1, 0}, {0xA6, {0x2A}, 1, 0},
    {0x87, {0x46}, 1, 0}, {0xA7, {0x44}, 1, 0},
    {0x83, {0x39}, 1, 0}, {0xA3, {0x39}, 1, 0},
    {0x84, {0x3A}, 1, 0}, {0xA4, {0x3A}, 1, 0},
    {0x85, {0x3F}, 1, 0}, {0xA5, {0x3F}, 1, 0},
    {0x88, {0x08}, 1, 0}, {0xA8, {0x08}, 1, 0},
    {0x89, {0x0F}, 1, 0}, {0xA9, {0x0F}, 1, 0},
    {0x8A, {0x17}, 1, 0}, {0xAA, {0x17}, 1, 0},
    {0x8B, {0x10}, 1, 0}, {0xAB, {0x10}, 1, 0},
    {0x8C, {0x16}, 1, 0}, {0xAC, {0x16}, 1, 0},
    {0x8D, {0x14}, 1, 0}, {0xAD, {0x14}, 1, 0},
    {0x8E, {0x11}, 1, 0}, {0xAE, {0x11}, 1, 0},
    {0x8F, {0x14}, 1, 0}, {0xAF, {0x14}, 1, 0},
    {0x90, {0x06}, 1, 0}, {0xB0, {0x06}, 1, 0},
    {0x91, {0x0F}, 1, 0}, {0xB1, {0x0F}, 1, 0},
    {0x92, {0x16}, 1, 0}, {0xB2, {0x16}, 1, 0},
    {0xFF, {0x00}, 1, 0},
    {0x11, {0}, 0, 120},
    {0x29, {0}, 0, 100},
};

static void nv3041a_init(void)
{
    for (size_t i = 0; i < sizeof(nv3041a_init_cmds) / sizeof(nv3041a_init_cmds[0]); i++) {
        const struct nv_cmd *c = &nv3041a_init_cmds[i];
        nv3041a_cmd(c->cmd, c->data, c->len);
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }
}

/* ---------- 显示运行时开关 ---------- */
/* 面板初始化的 0xC0 是本板与丝印一致的正常方向。 */
static uint8_t disp_madctl = 0xC0;
static bool disp_rot180;
static bool disp_mirrorx;

bool bsp_disp_can_invert(void)    { return true; }
bool bsp_disp_can_rotate180(void) { return true; }
bool bsp_disp_can_mirror_x(void)  { return true; }

void bsp_disp_set_invert(bool en)
{
    /* NV3041A 为 IPS 面板，INVON 才是正常颜色；设置项语义在此取反。 */
    nv3041a_cmd(en ? 0x20 : 0x21, NULL, 0);
}

void bsp_disp_set_rotate180(bool en)
{
    disp_rot180 = en;
    disp_madctl = 0xC0 ^ (disp_rot180 ? 0xC0 : 0x00)
                        ^ (disp_mirrorx ? 0x40 : 0x00);
    nv3041a_cmd(NV3041A_MADCTL, &disp_madctl, 1);
}

void bsp_disp_set_mirror_x(bool en)
{
    disp_mirrorx = en;
    disp_madctl = 0xC0 ^ (disp_rot180 ? 0xC0 : 0x00)
                        ^ (disp_mirrorx ? 0x40 : 0x00);
    nv3041a_cmd(NV3041A_MADCTL, &disp_madctl, 1);
}

/* ---------- LVGL 对接 ---------- */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    int32_t n = w * h;

    /* QSPI 要求先发高字节，LVGL 内存是小端 RGB565 → 就地交换 */
    uint16_t *p = (uint16_t *)px_map;
    for (int32_t i = 0; i < n; i++)
        p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));

    nv3041a_set_window(area->x1, area->y1, area->x2, area->y2);
    nv3041a_cmd(NV3041A_RAMWR, NULL, 0);

    nv3041a_push_pixels(p, n);

    lv_display_flush_ready(disp);
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

    /* 背光 LEDC PWM */
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

    /* CS 手动控制 */
    gpio_config_t cs_conf = {
        .pin_bit_mask = 1ULL << PIN_LCD_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cs_conf));
    gpio_set_level(PIN_LCD_CS, 1);

    /* 分配 QSPI DMA 对齐缓冲（16 字节对齐，1024 像素） */
    qspi_tx_buf = (uint16_t *)heap_caps_aligned_alloc(16,
                                                       QSPI_MAX_PIXELS * 2,
                                                       MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(qspi_tx_buf ? ESP_OK : ESP_ERR_NO_MEM);

    /* SPI 总线（QSPI 4 线） */
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_LCD_D0,
        .miso_io_num = PIN_LCD_D1,
        .sclk_io_num = PIN_LCD_SCK,
        .quadwp_io_num = PIN_LCD_D2,
        .quadhd_io_num = PIN_LCD_D3,
        .max_transfer_sz = QSPI_MAX_PIXELS * 2 + 16,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS
                 | SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* QSPI 设备：command=8bit, address=24bit, half-duplex, CS 手动 */
    spi_device_interface_config_t devcfg = {
        .command_bits = 8,
        .address_bits = 24,
        .dummy_bits   = 0,
        .mode         = 0,
        .clock_speed_hz = LCD_PCLK_HZ,
        .spics_io_num = -1,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &qspi_dev));

    /* NV3041A 初始化 */
    nv3041a_init();
    ESP_LOGI(TAG, "NV3041A init done (%d cmds)",
             (int)(sizeof(nv3041a_init_cmds) / sizeof(nv3041a_init_cmds[0])));

    /* 触摸 GT911（I2C0） */
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
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &touch_handle));

    /* LVGL：partial 缓冲 */
    lv_init();
    lv_tick_set_cb(tick_cb);

    size_t buf_px = LCD_H_RES * 40;
    disp_draw_buf = heap_caps_malloc(buf_px * sizeof(lv_color_t),
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!disp_draw_buf) {
        disp_draw_buf = heap_caps_malloc(buf_px * sizeof(lv_color_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!disp_draw_buf) {
        ESP_LOGE(TAG, "LVGL buffer alloc failed");
        return;
    }

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_buffers(disp, disp_draw_buf, NULL,
                           buf_px * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    const bsp_sleep_button_cfg_t sleep_btns[] = {{ PIN_BTN_BOOT, true }};
    ESP_ERROR_CHECK(bsp_sleep_button_init(sleep_btns, 1));

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "BSP ready (JC4827W543C, %dx%d, QSPI NV3041A)", LCD_H_RES, LCD_V_RES);
}

/* 开机动画推屏 */
void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *px)
{
    if (!px) return;

    size_t n = (size_t)w * h;
    uint16_t *copy = heap_caps_malloc(n * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!copy) return;
    for (size_t i = 0; i < n; i++)
        copy[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));

    nv3041a_set_window(x, y, x + w - 1, y + h - 1);
    nv3041a_cmd(NV3041A_RAMWR, NULL, 0);
    nv3041a_push_pixels(copy, n);
    free(copy);
}

void bsp_lcd_stats_print(void) { }

void bsp_restart(void)
{
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

#endif /* CONFIG_BOARD_JC4827W543C */