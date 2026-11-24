import re

def parse_font(path):
    txt = open(path, encoding='utf-8', errors='ignore').read()

    lists = {}
    for m in re.finditer(r'static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};', txt, re.S):
        lists[m.group(1)] = [int(h, 16) for h in re.findall(r'0x[0-9a-fA-F]+', m.group(2))]

    # cmaps: {range_start, range_length, glyph_id_start, unicode_list, type}
    cps = set()
    for m in re.finditer(
        r'\.range_start = (\d+), \.range_length = (\d+),\s*\.glyph_id_start = (\d+),\s*'
        r'\.unicode_list = (\w+), \.glyph_id_ofs_list = \w+, \.list_length = (\d+),\s*'
        r'\.type = (LV_FONT_FMT_TXT_CMAP_\w+)', txt, re.S):
        rs, rl, _gs, lname, _llen, typ = m.groups()
        rs, rl = int(rs), int(rl)
        if typ.endswith('FORMAT0_TINY'):
            cps.update(range(rs, rs + rl))
        elif lname in lists:
            for off in lists[lname]:
                cps.add(rs + off)
    return cps

lvl1 = set()
for hi in range(0xB0, 0xD8):
    for lo in range(0xA1, 0xFF):
        try:
            lvl1.add(ord(bytes([hi, lo]).decode('gb2312')))
        except Exception:
            pass

s16 = parse_font('ui_font_cjk_16.c')
s24 = parse_font('ui_font_cjk_24.c')

for name, s in (('16px', s16), ('24px', s24)):
    han = {c for c in s if 0x4E00 <= c <= 0x9FFF}
    print(f'{name}: total={len(s)} han={len(han)} misses_gb2312_lvl1={len(lvl1 - s)}')

names = ("中国平安 五粮液 英伟达 贵州茅台 宁德时代 比亚迪 招商银行 隆基绿能 药明康德 "
         "立讯精密 长江电力 紫金矿业 中芯国际 工业富联 京东方 三一重工 恒瑞医药 海康威视 "
         "泸州老窖 洋河股份 伊利股份 山西汾酒 通威股份 阳光电源 亿纬锂能 赣锋锂业 天齐锂业 "
         "韦尔股份 兆易创新 卓胜微 北方华创 中微公司 澜起科技 上证指数 深证成指 创业板指")
for name, s in (('16px', s16), ('24px', s24)):
    miss = sorted({c for c in names if '\u4e00' <= c <= '\u9fff' and ord(c) not in s})
    print(f'{name} missing in sample: {"".join(miss) or "(none)"}')
