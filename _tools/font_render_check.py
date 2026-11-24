import re, sys
from PIL import Image

path = sys.argv[1]
chars = sys.argv[2] if len(sys.argv) > 2 else '中国平安'

txt = open(path, encoding='utf-8', errors='ignore').read()
lists = {}
for m in re.finditer(r'static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};', txt, re.S):
    lists[m.group(1)] = [int(h, 16) for h in re.findall(r'0x[0-9a-fA-F]+', m.group(2))]
cmaps = []
for m in re.finditer(
        r'\.range_start = (\d+), \.range_length = (\d+),\s*\.glyph_id_start = (\d+),\s*'
        r'\.unicode_list = (\w+), \.unicode_list = \w+', txt, re.S):
    pass
for m in re.finditer(
        r'\.range_start = (\d+), \.range_length = (\d+),\s*\.glyph_id_start = (\d+),\s*'
        r'\.unicode_list = (\w+),\s*\.glyph_id_ofs_list = \w+, \.list_length = (\d+),\s*'
        r'\.type = (LV_FONT_FMT_TXT_CMAP_\w+)', txt, re.S):
    cmaps.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4), m.group(6)))
mb = re.search(r'static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc\[\] = \{(.*?)\n\};', txt, re.S)
items = re.findall(
    r'\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}',
    mb.group(1))
mbit = re.search(r'static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap\[\] = \{(.*?)\n\};', txt, re.S)
bitmap = bytes(int(h, 16) for h in re.findall(r'0x[0-9a-fA-F]{2}', mbit.group(1)))
print('bitmap', len(bitmap), 'glyphs', len(items))

def gid_of(cp):
    for rs, rl, gs, lname, typ in cmaps:
        if typ.endswith('FORMAT0_TINY'):
            if rs <= cp < rs + rl:
                return gs + (cp - rs)
        else:
            arr = lists.get(lname, [])
            if rs <= cp < rs + rl and (cp - rs) in arr:
                return gs + arr.index(cp - rs)
    return None

tiles = []
for ch in chars:
    g = gid_of(ord(ch))
    if g is None:
        print('missing', ch); continue
    bi, adv, bw, bh, ox, oy = map(int, items[g])
    img = Image.new('L', (bw, bh), 0)
    px = []
    for k in range(bw * bh):
        byte = bitmap[bi + k // 2]
        px.append((byte >> 4) if k % 2 == 0 else (byte & 0xF))
    img.putdata([v * 17 for v in px])
    tiles.append((ch, img))
    print(f'{ch}: box={bw}x{bh} ofs=({ox},{oy}) adv={adv}')

if tiles:
    W = sum(t[1].width + 6 for t in tiles) + 6
    H = max(t[1].height for t in tiles) + 12
    canvas = Image.new('L', (W, H), 0)
    x = 6
    for ch, im in tiles:
        canvas.paste(im, (x, 6))
        x += im.width + 6
    canvas = canvas.resize((W * 4, H * 4), Image.NEAREST)
    canvas.save('font_preview.png')
    print('saved font_preview.png')
