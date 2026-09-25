/*
 * 关于：项目元信息（项目名 / 作者 / 版本 / 板型 / 仓库 / 协议 / 框架）。
 * 长值条目键名单独一行、值占第二行，避免与键名挤在同一行。
 */
#include "../theme.h"
#include "../lang.h"
#include "../panel_mgr.h"
#include "../ui_nav.h"
#include "version.h"
#include "bsp.h"

/* 单行键值行：键左值右（沿用设置行样式） */
static int row1(lv_obj_t *scr, const char *key, const char *val, int y)
{
    theme_row(scr, key, val, y);
    return ui_px(39);
}

/* 两行键值行：键名第一行、长值第二行独占整宽 */
static int row2(lv_obj_t *scr, const char *key, const char *val, int y)
{
    lv_obj_t *row = theme_card(scr);
    lv_obj_set_size(row, ui_content_w(), ui_px(56));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);

    lv_obj_t *k = theme_label(row, key, THEME_FONT_M, THEME_COL_TEXT);
    lv_obj_align(k, LV_ALIGN_TOP_LEFT, ui_px(2), ui_px(4));
    lv_obj_t *v = theme_label(row, val, THEME_FONT_S, THEME_COL_TEXT_DIM);
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, ui_px(2), ui_px(28));
    return ui_px(57);
}

static lv_obj_t *create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, theme_col(THEME_COL_BG), 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);

    int y = THEME_TITLEBAR_H + ui_px(4);

    y += row2(scr, "项目", "KlipperScreen-esp", y);
    y += row1(scr, "作者", "umeko", y);
    y += row1(scr, "版本", KR_VERSION, y);
    y += row2(scr, "板型", bsp_board_name(), y);
    y += row2(scr, "GitHub", "umeiko/KlipperScreen-esp", y);
    y += row1(scr, "协议", "MIT License", y);
    y += row2(scr, "框架", "ESP-IDF v5.5.5 · LVGL 9.3", y);

    /* 纯列表页：左 = 返回（ui_nav 白名单） */
    ui_nav_group_set_list(lv_group_get_default(), true);
    return scr;
}

panel_def_t panel_about_def = {
    .name = "about", .title = "关于",
    .create = create,
    .on_show = NULL,
    .on_tick = NULL,
    .hide_temps = 1,
};
