/*
 * 文件列表：server.files.list 实时拉取（desktop 走 mock）。
 * 点文件名进二级菜单（file_detail：打印/删除），不直接打印。
 *
 * 历史文件很多时的防护（issue #8：esp32 无 PSRAM 机型上千条记录曾直接重启）：
 *  - 行分批构建：每个 LVGL 节拍只追加一小批，不把 lvgl 任务卡到 TWDT；
 *  - 行数上限：每行 4 个 LVGL 对象，堆放不下就截断并在末尾提示，防 OOM。
 */
#include "../theme.h"
#include "../ui_anim.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "printer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* panel_file_detail.c 提供：选中文件后打开二级菜单 */
void panel_file_detail_set(const char *name, uint32_t size, double modified);

/* 无 PSRAM 的小内存机型（CYD 等，开页时可用堆只有几十 KB）必须限制行数；
 * 有 PSRAM 的机型与桌面端放宽。超出上限截断并在末尾提示。 */
#if defined(ESP_PLATFORM) && !defined(CONFIG_SPIRAM)
#define FILES_MAX_ROWS     100
#else
#define FILES_MAX_ROWS     500
#endif
#define FILES_BUILD_BATCH  16   /* 每节拍追加行数：500 行约 1s 建完，界面保持可动 */
#define FILES_BUILD_PERIOD 30   /* ms */

static lv_obj_t *list;
static printer_file_t *files;   /* 缓存的列表（数据层回调移交所有权） */
static int file_count = -1;     /* -1 = 尚未拉取；0 = 空；>0 = 条数 */
static lv_timer_t *build_timer;
static int build_idx;           /* 已构建行数 */

static void build_stop(void)
{
    if (build_timer) {
        lv_timer_delete(build_timer);
        build_timer = NULL;
    }
}

static void show_status(const char *text)
{
    if (!list) return;
    build_stop();
    lv_obj_clean(list);
    lv_obj_t *l = theme_label(list, text, THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_set_width(l, ui_content_w());
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

static void on_row(lv_event_t *e);

static void append_row(int i)
{
    int row_w = ui_content_w();   /* 行撑满列表宽（list 无内边距），大屏不右侧留白 */
    lv_obj_t *row = theme_action_card(list);
    lv_obj_set_size(row, row_w, ui_px(40));
    lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *ic = theme_label(row, LV_SYMBOL_FILE, THEME_FONT_ICON, THEME_COL_ACCENT);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *name = theme_label(row, files[i].name, THEME_FONT_S, THEME_COL_TEXT);
    lv_obj_set_width(name, row_w - 2 * THEME_PAD - ui_px(24) - ui_px(70));
    lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, ui_px(24), 0);

    char sz[20];
    if (files[i].size >= 1024 * 1024) {
        theme_fmt_float(sz, sizeof(sz), files[i].size / 1048576.0f, 1);
        strncat(sz, "MB", sizeof(sz) - strlen(sz) - 1);
    } else {
        snprintf(sz, sizeof(sz), "%uKB", (unsigned)(files[i].size / 1024 + 1));
    }
    lv_obj_t *size = theme_label(row, sz, THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(size, LV_ALIGN_RIGHT_MID, -ui_px(4), 0);
}

static void build_step(lv_timer_t *t)
{
    (void)t;
    if (!list || !files) { build_stop(); return; }   /* 面板已销毁 */
    int shown = file_count < FILES_MAX_ROWS ? file_count : FILES_MAX_ROWS;
    int end = build_idx + FILES_BUILD_BATCH;
    if (end > shown) end = shown;
    for (; build_idx < end; build_idx++) append_row(build_idx);
    if (build_idx >= shown) {
        build_stop();
        if (file_count > FILES_MAX_ROWS) {
            char hint[64];
            snprintf(hint, sizeof(hint), "仅显示前 %d 个 · 共 %d 个文件", FILES_MAX_ROWS, file_count);
            lv_obj_t *l = theme_label(list, hint, THEME_FONT_S, THEME_COL_TEXT_DIM);
            lv_obj_set_width(l, ui_content_w());
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        }
    }
}

static void rebuild_rows(void)
{
    build_stop();
    if (!list) return;
    if (file_count <= 0) {
        show_status(file_count == 0 ? "暂无 GCode 文件" : "加载中…");
        return;
    }
    lv_obj_clean(list);
    build_idx = 0;
    build_timer = lv_timer_create(build_step, FILES_BUILD_PERIOD, NULL);
}

/* 面板不常驻：离开即销毁。销毁后静态指针一律失效，
 * 在途回调/分批定时器不得再碰界面（曾经历过离屏后回调踩野指针的风险点） */
static void on_list_delete(lv_event_t *e)
{
    (void)e;
    build_stop();
    list = NULL;
}

static void on_files(printer_file_t *f, int count, void *ud)
{
    (void)ud;
    free(files);
    if (count < 0) {   /* 获取失败（RPC 错误/解析失败/应答丢失），区别于空列表 */
        files = NULL;
        file_count = 0;
        show_status("获取失败，请检查连接");
        return;
    }
    files = f;
    file_count = count;
    rebuild_rows();
}

static void on_row(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= file_count || !files) return;
    panel_file_detail_set(files[idx].name, files[idx].size, files[idx].modified);
    panel_mgr_open("file_detail");
}

static void on_show(void)
{
    if (!printer_files_refresh(on_files, NULL)) {
        /* 未发出：离线（或上一请求在途，保留现有内容等它回来） */
        if (printer_state() == PRINTER_STATE_DISCONNECTED && file_count <= 0) {
            file_count = 0;
            show_status("未连接 Moonraker");
        }
    } else if (file_count < 0) {
        show_status("加载中…");
    }
    /* 有缓存时先展示旧数据，新数据到达后 on_files 重建 */
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);

    list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, ui_content_w(), ui_scr_h() - THEME_TITLEBAR_H - ui_px(12));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, THEME_TITLEBAR_H + ui_px(4));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, THEME_GAP, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(list, on_list_delete, LV_EVENT_DELETE, NULL);

    /* 纯列表页：左 = 返回、右 = 进入/确定（ui_nav 白名单） */
    ui_nav_group_set_list(lv_group_get_default(), true);
    return scr;
}

panel_def_t panel_files_def = {
    .name = "files", .title = "打印文件", .title_s = "文件",
    .create = create,
    .on_show = on_show,
    .on_tick = NULL,
    .hide_temps = 1,   /* 文件管理与打印控制无关，标题栏不显示温度 */
};
