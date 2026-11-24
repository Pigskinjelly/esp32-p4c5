import re, sys

def load(path):
    txt = open(path, encoding='utf-8', errors='ignore').read()
    lists = {}
    for m in re.finditer(r'static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};', txt, re.S):
        lists[m.group(1)] = [int(h, 16) for h in re.findall(r'0x[0-9a-fA-F]+', m.group(2))]
    cmaps = []
    for m in re.finditer(
            r'\.range_start = (\d+), \.range_length = (\d+),\s*\.glyph_id_start = (\d+),\s*'
            r'\.unicode_list = (\w+), \.glyph_id_ofs_list = \w+, \.list_length = (\d+),\s*'
            r'\.type = (LV_FONT_FMT_TXT_CMAP_\w+)', txt, re.S):
        cmaps.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4), m.group(6)))
    mb = re.search(r'static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc\[\] = \{(.*?)\n\};', txt, re.S)
    items = re.findall(
        r'\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}',
        mb.group(1))
    return lists, cmaps, items

def gid_of(cp, lists, cmaps):
    for rs, rl, gs, lname, typ in cmaps:
        if typ.endswith('FORMAT0_TINY'):
            if rs <= cp < rs + rl:
                return gs + (cp - rs)
        else:
            arr = lists.get(lname, [])
            if rs <= cp < rs + rl and (cp - rs) in arr:
                return gs + arr.index(cp - rs)
    return None

old = load('ui_font_cjk_24.c')
new = load(sys.argv[1])

print(f"{'ch':<3} {'old box/ofs':<22} {'new box/ofs':<22} {'old adv':>7} {'new adv':>7}")
for ch in '中国平安五粮液ABC123':
    cp = ord(ch)
    g1 = gid_of(cp, old[0], old[1])
    g2 = gid_of(cp, new[0], new[1])
    o = old[2][g1] if g1 is not None else None
    n = new[2][g2] if g2 is not None else None
    if o and n:
        print(f"{ch:<3} {o[2]}x{o[3]} @({o[4]},{o[5]})".ljust(24) +
              f" {n[2]}x{n[3]} @({n[4]},{n[5]})".ljust(24) +
              f" {o[1]:>7} {n[1]:>7}")
    else:
        print(f"{ch:<3} old={o} new={n}")
