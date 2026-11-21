/*
 * GBK(GB2312) → UTF-8 转换（行情接口返回的股票名是 GBK 编码）
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 把 GBK/GB2312 字符串转成 UTF-8，返回输出字节数；无法识别的字用 '?' 代替 */
int gbk_to_utf8(const char *in, char *out, size_t outsz);

/** 把 UTF-8 字符串转成 GBK/GB2312（FATFS 文件名用），返回字节数 */
int utf8_to_gbk(const char *in, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif
