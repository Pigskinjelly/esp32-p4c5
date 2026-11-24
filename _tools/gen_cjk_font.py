#!/usr/bin/env python3
"""生成 LVGL 8/9 兼容的 CJK 字体 C 文件（4bpp，稀疏 cmap）。

用法:
  python gen_cjk_font.py --size 24 --out ui_font_cjk_24.c --symbol ui_cjk_24
  python gen_cjk_font.py --size 24 --limit 200 --out /tmp/sample.c   # 仅前 N 个汉字，做格式校验

字符集: ASCII 32-126 + GB2312 全集(6763) + 常用符号/全角标点。
"""
import argparse
import os
from PIL import Image, ImageDraw, ImageFont

TTF = r"C:\Windows\Fonts\msyh.ttc"

# 额外补充：中英文标点、单位、常用符号、箭头、圆点、货币
EXTRA = (
    "·—…“”‘’【】《》〈〉「」『』（）〔〕％‰℃℉±×÷≈≠≤≥∞°′″"
    "①②③④⑤⑥⑦⑧⑨⑩★☆●○◆◇■□▲△▼▽←→↑↓↔⇒"
    "￥$€£¥§¶©®™µΩαβγπΣ"
    "０１２３４５６７８９"
)


def gb2312_chars(part="all"):
    """part: all | l1(一级 0xB0-0xD7) | l2(二级 0xD8-0xF7)"""
    out = []
    rng = [(0xA1, 0xF8)] if part == "all" else (
        [(0xB0, 0xD8)] if part == "l1" else [(0xD8, 0xF8)])
    for hi0, hi1 in rng:
        for hi in range(hi0, hi1):
            for lo in range(0xA1, 0xFF):
                try:
                    ch = bytes([hi, lo]).decode("gb2312")
                except Exception:
                    continue
                out.append(ch)
    return out


def build_charset(limit=0, part="all"):
    s = set()
    if part != "l2":
        s.update(chr(c) for c in range(32, 127))
        s.update(EXTRA)
    s.update(gb2312_chars(part))
    s = {c for c in s if ord(c) >= 32}
    cps = sorted(ord(c) for c in s)
    if limit:
        han = [c for c in cps if 0x4E00 <= c <= 0x9FFF][:limit]
        cps = [c for c in cps if c < 0x4E00 or c in han]
    return cps


def render(font, ch, bpp=4):
    """返回 (bitmap_bytes, adv_w, box_w, box_h, ofs_x, ofs_y)

    用 "ls"（left-baseline）锚点取包围盒，这样坐标与 LVGL 的
    「相对基线、向上为正」约定能直接对应：ofs_y = -bbox_bottom。
    """
    bbox = font.getbbox(ch, anchor="ls")
    x0, y0, x1, y1 = bbox
    if x1 <= x0 or y1 <= y0:
        adv = font.getlength(ch)
        return b"", round(adv * 16), 0, 0, 0, 0

    w, h = x1 - x0, y1 - y0
    img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(img)
    d.text((-x0, -y0), ch, fill=255, font=font, anchor="ls")

    px = list(img.getdata())
    out = bytearray()
    if bpp == 4:
        acc = 0
        n = 0
        for v in px:
            acc = (acc << 4) | (v >> 4)
            n += 1
            if n == 2:
                out.append(acc)
                acc = 0
                n = 0
        if n:
            out.append(acc << 4)
    else:  # bpp == 2
        acc = 0
        n = 0
        for v in px:
            acc = (acc << 2) | (v >> 6)
            n += 1
            if n == 4:
                out.append(acc)
                acc = 0
                n = 0
        if n:
            out.append(acc << (8 - 2 * n))

    adv = font.getlength(ch)
    return bytes(out), round(adv * 16), w, h, x0, -y1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--symbol", default=None)
    ap.add_argument("--line-height", type=int, default=0)
    ap.add_argument("--base-line", type=int, default=0)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--part", choices=["all", "l1", "l2"], default="all")
    ap.add_argument("--fallback", default=None,
                    help="主字体缺字时回退到的字体符号名")
    ap.add_argument("--bpp", type=int, default=4)
    ap.add_argument("--prefix", default="glyph", help="内部数组名前缀，避免同文件冲突")
    args = ap.parse_args()

    size = args.size
    symbol = args.symbol or f"ui_cjk_{size}"
    line_h = args.line_height or (size + 1)
    base_l = args.base_line or 4

    font = ImageFont.truetype(TTF, size, index=0)
    cps = build_charset(args.limit, args.part)
    print(f"chars={len(cps)} size={size} part={args.part} bpp={args.bpp}")

    glyphs = []       # (bitmap, adv, w, h, ox, oy)
    bitmap = bytearray()
    for cp in cps:
        bm, adv, w, h, ox, oy = render(font, chr(cp), args.bpp)
        glyphs.append((len(bitmap), adv, w, h, ox, oy))
        bitmap += bm

    print(f"bitmap={len(bitmap)} bytes")

    # 稀疏 cmap：range_start 取第一个码点，偏移为 uint16
    rs = cps[0]
    offs = [cp - rs for cp in cps]
    assert max(offs) < 65536, "offset overflow"

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        w = f.write
        w("/*\n * 自动生成：Microsoft YaHei %dpx, 4bpp, GB2312 全集\n * 生成脚本 _tools/gen_cjk_font.py\n */\n" % size)
        w('#include "lvgl.h"\n\n')
        w("#if LVGL_VERSION_MAJOR >= 8\n")
        w("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")
        for i in range(0, len(bitmap), 16):
            f.write("    " + ", ".join(f"0x{b:02x}" for b in bitmap[i:i + 16]) + ",\n")
        w("};\n\n")

        w("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
        w("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},\n")
        for bi, adv, bw, bh, ox, oy in glyphs:
            w(f"    {{.bitmap_index = {bi}, .adv_w = {adv}, .box_w = {bw}, .box_h = {bh}, "
              f".ofs_x = {ox}, .ofs_y = {oy}}},\n")
        w("};\n\n")

        w("static const uint16_t unicode_list_1[] = {\n")
        for i in range(0, len(offs), 12):
            f.write("    " + ", ".join(f"0x{o:x}" for o in offs[i:i + 12]) + ",\n")
        w("};\n\n")

        w("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
        w("    {\n")
        w(f"        .range_start = {rs}, .range_length = {max(offs) + 1}, .glyph_id_start = 1,\n")
        w(f"        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, "
          f".list_length = {len(offs)},\n")
        w("        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n")
        w("    }\n};\n\n")

        w("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
        w("    .glyph_bitmap = glyph_bitmap,\n")
        w("    .glyph_dsc = glyph_dsc,\n")
        w("    .cmaps = cmaps,\n")
        w("    .kern_dsc = NULL,\n    .kern_scale = 0,\n")
        w(f"    .cmap_num = 1,\n    .bpp = {args.bpp},\n    .kern_classes = 0,\n    .bitmap_format = 0,\n")
        w("};\n\n")

        if args.fallback:
            w(f"extern const lv_font_t {args.fallback};\n\n")
        w(f"const lv_font_t {symbol} = {{\n")
        w("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
        w("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
        w(f"    .line_height = {line_h},\n")
        w(f"    .base_line = {base_l},\n")
        w("    .subpx = LV_FONT_SUBPX_NONE,\n")
        w("    .underline_position = -2,\n    .underline_thickness = 1,\n")
        w("    .dsc = &font_dsc,\n")
        w(f"    .fallback = {('&' + args.fallback) if args.fallback else 'NULL'},\n    .user_data = NULL,\n}};\n")
        w("#endif\n")

    print("wrote", args.out, os.path.getsize(args.out), "bytes")


if __name__ == "__main__":
    main()
