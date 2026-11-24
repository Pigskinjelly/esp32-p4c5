import re, sys

txt = open('ui_font_cjk_24.c', encoding='utf-8', errors='ignore').read()

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
print('items', len(items))

def gid_of(cp):
    for rs, rl, gs, lname, typ in cmaps:
        if typ.endswith('FORMAT0_TINY'):
            if rs <= cp < rs + rl:
                return gs + (cp - rs)
        else:
            arr = lists[lname]
            if rs <= cp < rs + rl:
                off = cp - rs
                if off in arr:
                    return gs + arr.index(off)
    return None

for ch in '中国平安五粮液':
    g = gid_of(ord(ch))
    if g is None:
        print(ch, 'MISSING'); continue
    it = items[g]
    bi, aw, bw, bh, ox, oy = map(int, it)
    print(f'{ch} U+{ord(ch):04X} gid={g} bitmap_index={bi} adv_w={aw} box={bw}x{bh} ofs=({ox},{oy}) bytes={bh*((bw+1)//2)}')

# 验证相邻 bitmap_index 差值规则
bad = 0
for i in range(1, len(items) - 1):
    bi, aw, bw, bh, ox, oy = map(int, items[i])
    nbi = int(items[i + 1][0])
    exp = bh * ((bw + 1) // 2)
    if nbi - bi != exp:
        bad += 1
        if bad <= 3:
            print('MISMATCH at', i, 'idx', bi, nbi, 'exp', exp, 'box', bw, bh)
print('bitmap_index mismatches:', bad, '/', len(items) - 2)
