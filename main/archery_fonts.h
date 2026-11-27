// main/archery_fonts.h —— 应用字体入口。
//
// 中文字形来自 assets/fonts/archery_font_16.c / archery_font_20.c（Noto Sans SC
// 子集，OFL-1.1），由 tools/gen_lvgl_text_font.py 从 UI 字符串清单生成；数字
// 大字号沿用 LVGL 自带 Montserrat 28/40。字形覆盖由
// tests/test_archery_fonts.py 与运行期 audit 双重检查。
#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(archery_font_16);
LV_FONT_DECLARE(archery_font_20);
LV_FONT_DECLARE(archery_font_20_bold);

#define ARCHERY_FONT_BODY  (&archery_font_16)
#define ARCHERY_FONT_TITLE (&archery_font_20)
#define ARCHERY_FONT_BOLD  (&archery_font_20_bold)
#define ARCHERY_FONT_NUMBER (&lv_font_montserrat_20)
#define ARCHERY_FONT_BIG   (&lv_font_montserrat_40)

// 运行期字形自检：把 text 中每个码点在 font 里查一遍，缺失时以 "U+XXXX"
// 记录警告。界面里每条文字创建时都会调用，因此新增中文忘记重新生成字体时
// 串口日志会直接指出缺哪个字。
void archery_font_audit(const lv_font_t *font, const char *text);

// 诊断日志页含任意 ASCII，临时关闭缺字审计，避免刷屏。
void archery_font_audit_set_enabled(bool enabled);

// 把 font 缺字形的字符替换为 '?'，避免日志页出现方块。
void archery_font_filter(const lv_font_t *font, const char *text, char *out,
                         size_t capacity);
