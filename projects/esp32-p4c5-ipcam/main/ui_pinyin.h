/* 极简拼音输入支持：拼音 → 候选汉字 / 常用词 */
#pragma once

/** 返回该拼音对应的候选汉字串（UTF-8，可能为空），没有返回 NULL */
const char *ui_pinyin_chars(const char *py);

/**
 * 以 py 开头的常用词候选，多个词用 '|' 分隔（完全匹配排在最前）
 * 例：ui_pinyin_words("nih") → "你好|你们|..."
 * 也支持首字母缩写：ui_pinyin_words("twlw") → "天外来物|..."
 * 没有匹配返回 NULL。返回静态缓冲，用完即失效。
 */
const char *ui_pinyin_words(const char *py);

/** 该词对应的拼音字母数（用于选词后吃掉对应拼音），找不到返回 -1 */
int ui_pinyin_word_pylen(const char *word);

/**
 * 记忆候选：返回 "used|词|used|词|"（按使用频率排序），没有返回 NULL。
 * 用户打过的词/字会排在最前，实现"越打越顺手"。
 */
const char *ui_pinyin_learned(const char *py);

/** 记录一次上屏（py = 这次消耗的拼音，word = 上屏文字） */
void ui_pinyin_learn(const char *py, const char *word);

/** 是否是完整音节 */
int ui_pinyin_is_syllable(const char *py);

/** 该拼音的首选字（返回静态缓冲，用完即失效） */
const char *ui_pinyin_first(const char *py);
