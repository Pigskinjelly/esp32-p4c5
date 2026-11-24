import re
from collections import Counter

txt = open('ui_font_cjk_24.c', encoding='utf-8', errors='ignore').read()

# glyph_dsc 条目
m = re.search(r'static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc\[\] = \{(.*?)\n\};', txt, re.S)
body = m.group(1)
items = re.findall(r'\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}', body)
print('glyph count', len(items))
c = Counter((int(i[2]), int(i[3])) for i in items)
print('top box sizes:', c.most_common(6))

advs = Counter(int(i[1]) for i in items)
print('adv_w values:', advs.most_common(5))

ofsy = Counter(int(i[5]) for i in items)
print('ofs_y values:', ofsy.most_common(6))

# bitmap 前 64 字节
mb = re.search(r'static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap\[\] = \{(.*?)\n\};', txt, re.S)
bb = [int(h, 16) for h in re.findall(r'0x[0-9a-fA-F]{2}', mb.group(1))]
print('bitmap bytes', len(bb))
print('first 48 bytes:', ' '.join(f'{x:02x}' for x in bb[:48]))

# 最后一个字形结束位置，验证 bpp 行对齐公式
last = items[-1]
bi = int(last[0]); bw = int(last[2]); bh = int(last[3])
print('last item', last, 'expected bytes if 4bpp row-pad:', bh * ((bw + 1) // 2))
print('total bitmap', len(bb), 'last idx', bi)
