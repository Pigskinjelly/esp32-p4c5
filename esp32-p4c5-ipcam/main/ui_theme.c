/*
 * UI 主题通用小工具
 */
#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui_theme.h"
#include "ui_shell.h"
#include "ui_pinyin.h"

lv_obj_t *ui_card(lv_obj_t *parent, int w, int h, uint32_t bg)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, font ? font : &ui_cjk_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    return lbl;
}

void ui_app_topbar(lv_obj_t *parent, const char *title)
{
    /* 设计变更：App 内不再显示标题栏，内容直接顶到屏幕上方，
     * 由 open_app_idx 统一把内容上移 34px（见 app_shift_up）。 */
    (void)parent;
    (void)title;
}

const char *ui_font_safe(const char *utf8, const lv_font_t *font)
{
    static char buf[128];
    if (!utf8) {
        return "";
    }
    const lv_font_t *f = font ? font : &ui_cjk_16;
    size_t o = 0;
    const char *p = utf8;
    while (*p && o + 2 < sizeof(buf)) {
        if ((unsigned char)*p < 0x80) {
            buf[o++] = *p++;
            continue;
        }
        /* UTF-8 多字节字符：判断字体是否有该字形 */
        uint32_t cp = 0;
        int len = 0;
        unsigned char c0 = (unsigned char)p[0];
        if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; len = 2; }
        else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; len = 3; }
        else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; len = 4; }
        else { buf[o++] = '?'; p++; continue; }

        bool valid = true;
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        /* 注意：dsc_out 不能传 NULL —— LVGL 的 LV_CHECK_ARG 会直接返回 false，
         * 那样所有中文都会被误判为缺字而替换成 '?' */
        lv_font_glyph_dsc_t dsc;
        if (!valid || !lv_font_get_glyph_dsc(f, &dsc, cp, 0)) {
            buf[o++] = '?';
        } else {
            for (int i = 0; i < len; i++) {
                buf[o++] = p[i];
            }
        }
        p += len;
    }
    buf[o] = '\0';
    return buf;
}

/* ================= 自绘简易键盘 ================= */
/* 按键码：1..0x7F 就是字符本身；0x100 以上是功能键。
 * 之前把字符串指针塞进 user_data，而字符串放在共享静态池里，每次 build 都
 * 把池计数清零重写：拼音键盘先建（q w e…）、校准键盘后建（1 2 3…）会把
 * 前面十几个槽位盖掉 —— 点 q 出来 1，所以"键盘字母是错的"。
 * 改成整数码后，按钮之间不再共享任何字符串。 */
enum {
    KP_KEY_DEL = 0x100,
    KP_KEY_OK,
    KP_KEY_SP,
    KP_KEY_CN,
};

static char *s_kp_buf;
static size_t s_kp_cap;
static lv_obj_t *s_kp_lbl;
static void (*s_kp_change)(void);
static void (*s_kp_ok)(void);

static void kp_key_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_kp_buf) {
        return;
    }
    if (code == KP_KEY_DEL) {
        size_t n = strlen(s_kp_buf);
        if (n) {
            s_kp_buf[n - 1] = 0;
        }
    } else if (code == KP_KEY_OK) {
        if (s_kp_ok) {
            s_kp_ok();
        }
    } else {
        size_t n = strlen(s_kp_buf);
        if (n + 1 < s_kp_cap && code > 0 && code < 0x80) {
            s_kp_buf[n] = (char)code;
            s_kp_buf[n + 1] = 0;
        }
    }
    if (s_kp_lbl) {
        lv_label_set_text(s_kp_lbl, s_kp_buf);
    }
    if (s_kp_change) {
        s_kp_change();
    }
}

void ui_keypad_bind(char *buf, size_t cap, lv_obj_t *label,
                    void (*on_change)(void), void (*on_ok)(void))
{
    s_kp_buf = buf;
    s_kp_cap = cap;
    s_kp_lbl = label;
    s_kp_change = on_change;
    s_kp_ok = on_ok;
}

static lv_obj_t *kp_btn(lv_obj_t *parent, const char *label, int code,
                        int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, kp_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)code);
    lv_obj_t *l = ui_label(b, label, &ui_cjk_16, C_TXT);
    lv_obj_center(l);
    return b;
}

void ui_keypad_build(lv_obj_t *parent, int y, const char *rows[], int nrows)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, 480, 216);
    lv_obj_set_pos(panel, 0, y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x121821), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    int ypos = 6;
    for (int r = 0; r < nrows; r++) {
        const char *s = rows[r];
        int n = (int)strlen(s);
        if (n <= 0) {
            continue;
        }
        int gap = 5;
        int bw = (470 - gap * (n - 1)) / n;
        if (bw > 60) {
            bw = 60;
        }
        int total = bw * n + gap * (n - 1);
        int x = (480 - total) / 2;
        for (int i = 0; i < n; i++) {
            char one[2] = { s[i], 0 };
            kp_btn(panel, one, (int)(unsigned char)s[i],
                   x + i * (bw + gap), ypos, bw, 38, 0x1E2836);
        }
        ypos += 42;
    }

    /* 底部：空格 / 删除 / 确定 */
    kp_btn(panel, "空格", ' ', 10, ypos, 180, 38, 0x1E2836);
    kp_btn(panel, "删除", KP_KEY_DEL, 200, ypos, 120, 38, 0x3A2A2A);
    kp_btn(panel, "确定", KP_KEY_OK, 330, ypos, 140, 38, 0x2E7D5B);
}

/* ================= 文本键盘（英文 / 中文拼音） ================= */
static char s_py[24];
static int  s_cn_mode = 1;
static lv_obj_t *s_kp_cand;      /* 候选条：横向滚动，能翻到所有同音字 */
static lv_obj_t *s_kp_pybox;     /* 左侧固定的拼音显示框 */
static lv_obj_t *s_kp_py_lbl;
static lv_obj_t *s_kp_mode_lbl;

/* 候选按钮的参数：上屏文字 + 这条候选消耗几个拼音字母。
 * 用静态池而不是 kp_bytes 池，是为了同时带上 used 字段。 */
typedef struct {
    char txt[24];
    int  used;
} cand_t;
static cand_t s_cands[96];
static int s_cand_cnt;

static void kp_cand_refresh(void);

static void kp_append(const char *txt)
{
    if (!s_kp_buf || !txt) {
        return;
    }
    size_t n = strlen(s_kp_buf), add = strlen(txt);
    if (n + add + 1 >= s_kp_cap) {
        return;
    }
    memcpy(s_kp_buf + n, txt, add + 1);
    if (s_kp_lbl) {
        lv_label_set_text(s_kp_lbl, s_kp_buf);
    }
    if (s_kp_change) {
        s_kp_change();
    }
}

static void kp_del_char(void)
{
    if (!s_kp_buf) {
        return;
    }
    size_t n = strlen(s_kp_buf);
    if (!n) {
        return;
    }
    do {
        n--;
    } while (n > 0 && ((unsigned char)s_kp_buf[n] & 0xC0) == 0x80);
    s_kp_buf[n] = 0;
    if (s_kp_lbl) {
        lv_label_set_text(s_kp_lbl, s_kp_buf);
    }
    if (s_kp_change) {
        s_kp_change();
    }
}

/* UTF-8 字符长度 */
static int u8_len(unsigned char c0)
{
    if (c0 < 0x80) {
        return 1;
    }
    if ((c0 & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c0 & 0xF8) == 0xF0) {
        return 4;
    }
    return 2;
}

/* 一段连续拼音里，最长的合法音节长度："nihao" → 2（ni） */
static int first_syl_len(const char *py)
{
    int best = 0;
    char t[8];
    for (int n = 1; n <= 6 && py[n - 1]; n++) {
        memcpy(t, py, (size_t)n);
        t[n] = 0;
        if (ui_pinyin_is_syllable(t)) {
            best = n;      /* 继续往后试，取最长的 */
        }
    }
    return best;
}

/* 点候选（整词或单字）：上屏，并且只吃掉它对应的那段拼音。
 * 之前一次点击清空全部拼音，打"nihaoma"选"你好"会把"ma"也弄丢。 */
static void kp_cand_cb(lv_event_t *e)
{
    const cand_t *c = (const cand_t *)lv_event_get_user_data(e);
    if (!c) {
        return;
    }
    kp_append(c->txt);
    size_t n = strlen(s_py);
    int used = c->used;
    if (used > (int)n) {
        used = (int)n;
    }
    if (used > 0) {
        /* 记住这次：拼音 → 词，下次优先出 */
        char consumed[16];
        int cl = used > 15 ? 15 : used;
        memcpy(consumed, s_py, (size_t)cl);
        consumed[cl] = 0;
        ui_pinyin_learn(consumed, c->txt);
        memmove(s_py, s_py + used, n - (size_t)used + 1);
    }
    kp_cand_refresh();
}

/* 往候选条里加一个候选（候选条是 flex 横排 + 横向滚动） */
static void kp_cand_add(const char *txt, int used, int w, uint32_t bg)
{
    if (!s_kp_cand || s_cand_cnt >= (int)(sizeof(s_cands) / sizeof(s_cands[0]))) {
        return;
    }
    cand_t *c = &s_cands[s_cand_cnt++];
    strlcpy(c->txt, txt, sizeof(c->txt));
    c->used = used;

    lv_obj_t *b = lv_button_create(s_kp_cand);
    lv_obj_set_size(b, w, 38);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, kp_cand_cb, LV_EVENT_CLICKED, c);
    lv_obj_t *l = ui_label(b, txt, &ui_cjk_16, C_TXT);
    lv_obj_center(l);
}

static void kp_cand_hint(const char *txt, uint32_t color)
{
    if (!s_kp_cand) {
        return;
    }
    lv_obj_t *h = ui_label(s_kp_cand, txt, &ui_cjk_16, color);
    lv_obj_set_style_pad_left(h, 8, 0);
}

/* 候选条：
 *   [固定的拼音框] [整词…] [单字… 全部，可横向滚动/翻页]
 * 拼音框里的字母会一直留着，选完一个字只吃掉对应的那段拼音。
 */
static void kp_cand_refresh(void)
{
    if (!s_kp_cand) {
        return;
    }
    lv_obj_clean(s_kp_cand);
    s_cand_cnt = 0;

    /* 左：拼音框（固定，不跟着候选滚动） */
    bool composing = (s_cn_mode && s_py[0]);
    if (s_kp_py_lbl) {
        lv_label_set_text(s_kp_py_lbl, composing ? s_py : (s_cn_mode ? "拼音" : "EN"));
        lv_obj_set_style_text_color(s_kp_py_lbl,
                                    lv_color_hex(composing ? 0xFFD166 : 0x5B6B85), 0);
    }
    if (s_kp_pybox) {
        lv_obj_set_style_bg_color(s_kp_pybox, lv_color_hex(composing ? 0x243F63 : 0x151B26), 0);
        lv_obj_set_style_border_width(s_kp_pybox, composing ? 1 : 0, 0);
    }

    if (!s_cn_mode) {
        kp_cand_hint("English", 0x5B6B85);
        return;
    }
    if (!s_py[0]) {
        kp_cand_hint("打拼音字母 → 右侧滑/翻页选字", 0x5B6B85);
        return;
    }

    /* 0. 记忆：用户常打的词/字，排在最前面（绿色区分） */
    const char *ls = ui_pinyin_learned(s_py);
    if (ls) {
        const char *p = ls;
        while (*p && s_cand_cnt < 90) {
            char *end = NULL;
            int used = (int)strtol(p, &end, 10);
            if (!end || *end != '|') {
                break;
            }
            const char *w = end + 1;
            const char *bar = strchr(w, '|');
            int len = bar ? (int)(bar - w) : (int)strlen(w);
            if (len > 0) {
                char txt[24];
                if (len > 23) {
                    len = 23;
                }
                memcpy(txt, w, (size_t)len);
                txt[len] = 0;
                int nch = 0;
                for (int i = 0; i < len; i += u8_len((unsigned char)txt[i])) {
                    nch++;
                }
                bool dup = false;
                for (int k = 0; k < s_cand_cnt; k++) {
                    if (strcmp(s_cands[k].txt, txt) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup && nch > 0) {
                    kp_cand_add(txt, used, 14 + nch * 17, 0x2F5A3A);
                }
            }
            if (!bar) {
                break;
            }
            p = bar + 1;
        }
    }

    /* 1. 常用整词（前缀匹配，打一半也能整词上屏） */
    const char *ws = ui_pinyin_words(s_py);
    bool any_word = false;
    if (ws) {
        const char *p = ws;
        int nw = 0;
        while (*p && nw < 6) {
            const char *bar = strchr(p, '|');
            int len = bar ? (int)(bar - p) : (int)strlen(p);
            int nch = 0;
            for (int i = 0; i < len; i += u8_len((unsigned char)p[i])) {
                nch++;
            }
            if (nch > 0 && nch <= 6) {
                char txt[24];
                if (len > 23) len = 23;
                memcpy(txt, p, (size_t)len);
                txt[len] = 0;
                int used = ui_pinyin_word_pylen(txt);
                kp_cand_add(txt, used, 14 + nch * 17, 0x2B4A72);
                any_word = true;
                nw++;
            }
            if (!bar) {
                break;
            }
            p = bar + 1;
        }
    }

    /* 2. 首音节单字：给出全部同音字（横向滚动 + 翻页，不再只给 8 个） */
    int syl = first_syl_len(s_py);
    const char *cs = NULL;
    if (syl > 0 && syl < (int)sizeof(s_py)) {
        char tmp[8];
        memcpy(tmp, s_py, (size_t)syl);
        tmp[syl] = 0;
        cs = ui_pinyin_chars(tmp);
    }
    if (cs && cs[0]) {
        int i = 0;
        const char *p = cs;
        while (*p && i < 24) {
            int n = u8_len((unsigned char)*p);
            char buf[8];
            memcpy(buf, p, (size_t)n);
            buf[n] = 0;
            kp_cand_add(buf, syl, 46, 0x27374D);
            i++;
            p += n;
        }
    } else if (!any_word) {
        kp_cand_hint("无此拼音", 0xFF8A8A);
    }

    /* 每次输入回到最左边 */
    lv_obj_scroll_to_x(s_kp_cand, 0, LV_ANIM_OFF);
}

static void kp_text_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    if (code == KP_KEY_DEL) {
        if (s_cn_mode && s_py[0]) {
            s_py[strlen(s_py) - 1] = 0;
        } else {
            kp_del_char();
        }
        kp_cand_refresh();
    } else if (code == KP_KEY_OK) {
        if (s_kp_ok) {
            s_kp_ok();
        }
    } else if (code == KP_KEY_SP) {
        if (s_cn_mode) {
            if (s_py[0]) {
                /* 只上屏首音节的首字，剩下的拼音继续留着 */
                int syl = first_syl_len(s_py);
                char tmp[8];
                if (syl > 0 && syl < (int)sizeof(tmp)) {
                    memcpy(tmp, s_py, (size_t)syl);
                    tmp[syl] = 0;
                    const char *f = ui_pinyin_first(tmp);
                    if (f) {
                        kp_append(f);
                    }
                    memmove(s_py, s_py + syl, strlen(s_py) - (size_t)syl + 1);
                }
                kp_cand_refresh();
            } else {
                kp_append(" ");      /* 没在打拼音时空格就是空格 */
            }
        } else {
            kp_append(" ");
        }
    } else if (code == KP_KEY_CN) {
        s_cn_mode = !s_cn_mode;
        s_py[0] = 0;
        if (s_kp_mode_lbl) {
            lv_label_set_text(s_kp_mode_lbl, s_cn_mode ? "中" : "英");
        }
        kp_cand_refresh();
    } else if (code > 0 && code < 0x80) {
        if (s_cn_mode) {
            size_t n = strlen(s_py);
            if (n + 1 < sizeof(s_py)) {
                s_py[n] = (char)code;
                s_py[n + 1] = 0;
            }
            kp_cand_refresh();
        } else {
            char one[2] = { (char)code, 0 };
            kp_append(one);
        }
    }
}

static lv_obj_t *kp_tbtn(lv_obj_t *parent, const char *label, int code,
                         int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, kp_text_cb, LV_EVENT_CLICKED, (void *)(intptr_t)code);
    lv_obj_t *l = ui_label(b, label, &ui_cjk_16, C_TXT);
    lv_obj_center(l);
    return b;
}

/* 候选翻页：同音字多的时候靠这两个箭头翻（也可以直接在候选条上横向拖） */
static void kp_page_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_kp_cand) {
        return;
    }
    lv_coord_t x = lv_obj_get_scroll_x(s_kp_cand) + (lv_coord_t)(dir * 260);
    lv_obj_scroll_to_x(s_kp_cand, x, LV_ANIM_ON);
}

void ui_keypad_build_text(lv_obj_t *parent, int y)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, 480, 216);
    lv_obj_set_pos(panel, 0, y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x121821), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 拼音显示框（固定在最左，不参与滚动） */
    s_kp_pybox = lv_obj_create(panel);
    lv_obj_set_size(s_kp_pybox, 64, 38);
    lv_obj_set_pos(s_kp_pybox, 6, 4);
    lv_obj_set_style_bg_color(s_kp_pybox, lv_color_hex(0x151B26), 0);
    lv_obj_set_style_bg_opa(s_kp_pybox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kp_pybox, 0, 0);
    lv_obj_set_style_border_color(s_kp_pybox, lv_color_hex(0x4C7FB8), 0);
    lv_obj_set_style_radius(s_kp_pybox, 8, 0);
    lv_obj_set_style_pad_all(s_kp_pybox, 0, 0);
    lv_obj_clear_flag(s_kp_pybox, LV_OBJ_FLAG_SCROLLABLE);
    s_kp_py_lbl = ui_label(s_kp_pybox, "拼音", &ui_cjk_16, 0x5B6B85);
    lv_label_set_long_mode(s_kp_py_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_kp_py_lbl, 58);
    lv_obj_set_style_text_align(s_kp_py_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_kp_py_lbl);

    /* 候选字条：flex 横排 + 横向滚动，装得下所有同音字 */
    s_kp_cand = lv_obj_create(panel);
    lv_obj_set_size(s_kp_cand, 354, 46);
    lv_obj_set_pos(s_kp_cand, 100, 0);
    lv_obj_set_style_bg_color(s_kp_cand, lv_color_hex(0x0C1119), 0);
    lv_obj_set_style_bg_opa(s_kp_cand, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kp_cand, 0, 0);
    lv_obj_set_style_pad_all(s_kp_cand, 0, 0);
    lv_obj_set_style_pad_left(s_kp_cand, 4, 0);
    lv_obj_set_scroll_dir(s_kp_cand, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_kp_cand, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s_kp_cand, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_kp_cand, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_kp_cand, 4, 0);

    /* 翻页箭头：压在候选条两端 */
    lv_obj_t *pl = lv_button_create(panel);
    lv_obj_set_size(pl, 24, 46);
    lv_obj_set_pos(pl, 74, 0);
    lv_obj_set_style_bg_color(pl, lv_color_hex(0x1A2430), 0);
    lv_obj_set_style_bg_opa(pl, LV_OPA_80, 0);
    lv_obj_set_style_radius(pl, 0, 0);
    lv_obj_set_style_pad_all(pl, 0, 0);
    lv_obj_add_event_cb(pl, kp_page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(-1));
    lv_obj_t *pll = lv_label_create(pl);
    lv_label_set_text(pll, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(pll, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pll, lv_color_hex(0x9FB6D4), 0);
    lv_obj_center(pll);

    lv_obj_t *pr = lv_button_create(panel);
    lv_obj_set_size(pr, 24, 46);
    lv_obj_set_pos(pr, 456, 0);
    lv_obj_set_style_bg_color(pr, lv_color_hex(0x1A2430), 0);
    lv_obj_set_style_bg_opa(pr, LV_OPA_80, 0);
    lv_obj_set_style_radius(pr, 0, 0);
    lv_obj_set_style_pad_all(pr, 0, 0);
    lv_obj_add_event_cb(pr, kp_page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(1));
    lv_obj_t *prl = lv_label_create(pr);
    lv_label_set_text(prl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(prl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(prl, lv_color_hex(0x9FB6D4), 0);
    lv_obj_center(prl);

    static const char *rows[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    int ypos = 50;
    for (int r = 0; r < 3; r++) {
        const char *s = rows[r];
        int n = (int)strlen(s);
        int gap = 4;
        int bw = (464 - gap * (n - 1)) / n;
        if (bw > 48) bw = 48;
        int total = bw * n + gap * (n - 1);
        int x = (480 - total) / 2;
        for (int i = 0; i < n; i++) {
            char one[2] = { s[i], 0 };
            kp_tbtn(panel, one, (int)(unsigned char)s[i],
                    x + i * (bw + gap), ypos, bw, 38, 0x1E2836);
        }
        ypos += 42;
    }

    /* 底部：中/英 · 空格 · 删除 · 确定 */
    lv_obj_t *mb = kp_tbtn(panel, "中", KP_KEY_CN, 8, ypos, 60, 38, 0x2E4C7A);
    s_kp_mode_lbl = lv_obj_get_child(mb, 0);
    lv_label_set_text(s_kp_mode_lbl, s_cn_mode ? "中" : "英");
    kp_tbtn(panel, "空格", KP_KEY_SP, 74, ypos, 190, 38, 0x1E2836);
    kp_tbtn(panel, "删除", KP_KEY_DEL, 270, ypos, 100, 38, 0x3A2A2A);
    kp_tbtn(panel, "确定", KP_KEY_OK, 376, ypos, 96, 38, 0x2E7D5B);

    kp_cand_refresh();
}
